//! PhoneMic — native Windows GUI.
//!
//! A real OS window (via `eframe`/egui, no web/webview) that runs the UDP
//! receiver and audio output in a background thread and shows the user what they
//! need: the IP to type on the phone, live connection status, a mic-level meter,
//! and where the audio is being routed (VB-CABLE vs. speakers).

// No console window in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use eframe::egui;

use phonemic_core::denoise::Denoiser;
use phonemic_core::sink::AudioSink;
use phonemic_core::transport::{Transport, UdpTransport};
use phonemic_protocol::{decode, Codec};

const PORT: u16 = 4010;

/// State shared between the audio/receive thread and the UI.
#[derive(Default)]
struct Shared {
    peak_bits: AtomicU32, // latest packet peak level (f32 bits), 0.0..=1.0
    packets: AtomicU64,
    last_packet_ms: AtomicU64,
    is_cable: AtomicBool,
    noise_suppression: AtomicBool, // UI toggle → receiver thread
    dropped_encrypted: AtomicU64,  // encrypted packets we couldn't decrypt
    peer: Mutex<String>,
    device: Mutex<String>,
    pin: Mutex<String>, // shared security PIN (empty = no encryption)
    error: Mutex<Option<String>>,
}

impl Shared {
    fn set_peak(&self, v: f32) {
        self.peak_bits.store(v.to_bits(), Ordering::Relaxed);
    }
    fn peak(&self) -> f32 {
        f32::from_bits(self.peak_bits.load(Ordering::Relaxed))
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

/// Best-effort primary LAN IPv4 (no deps): "connect" a UDP socket toward a
/// public address and read which local interface the OS picks.
fn local_ip() -> String {
    (|| {
        let s = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
        s.connect("8.8.8.8:80").ok()?;
        match s.local_addr().ok()?.ip() {
            std::net::IpAddr::V4(v4) if !v4.is_loopback() => Some(v4.to_string()),
            _ => None,
        }
    })()
    .unwrap_or_else(|| "your PC's IP".to_string())
}

/// Receive + play audio on a dedicated thread (cpal's stream is not Send, so it
/// is created and owned here and never crosses a thread boundary).
fn spawn_receiver(shared: Arc<Shared>) {
    std::thread::spawn(move || {
        let transport = match UdpTransport::bind(([0, 0, 0, 0], PORT).into()) {
            Ok(t) => t,
            Err(e) => {
                *shared.error.lock().unwrap() = Some(format!("Couldn't open port {PORT}: {e}"));
                return;
            }
        };
        // Prefer VB-CABLE so the phone becomes a real mic; else speakers.
        let mut sink = match AudioSink::new(Some("cable")) {
            Ok(s) => s,
            Err(e) => {
                *shared.error.lock().unwrap() = Some(format!("No audio output: {e}"));
                return;
            }
        };
        *shared.device.lock().unwrap() = sink.device_name.clone();
        shared.is_cable.store(sink.is_virtual_cable, Ordering::Relaxed);

        let mut denoiser = Denoiser::new();
        let mut denoised: Vec<i16> = Vec::with_capacity(1024);
        let mut buf = [0u8; 2048];
        // Cache the derived key so we only recompute it when the PIN changes.
        let mut cur_pin = String::new();
        let mut key = phonemic_core::crypto::derive_key("");
        loop {
            let Ok((n, peer)) = transport.recv(&mut buf) else { continue };
            let header_bytes = if n >= 18 { buf[..18].to_vec() } else { continue };
            let Ok((header, payload)) = decode(&buf[..n]) else { continue };
            if header.codec != Codec::Pcm16 {
                continue;
            }

            // Decrypt if the sender marked the payload encrypted.
            let pcm_bytes: Vec<u8> = if header.encrypted {
                let pin = shared.pin.lock().unwrap().clone();
                if pin.is_empty() {
                    shared.dropped_encrypted.fetch_add(1, Ordering::Relaxed);
                    continue; // encrypted stream but no PIN set here
                }
                if pin != cur_pin {
                    key = phonemic_core::crypto::derive_key(&pin);
                    cur_pin = pin;
                }
                match phonemic_core::crypto::decrypt(
                    &key, &header_bytes, header.seq, header.timestamp_us, payload,
                ) {
                    Some(pt) => pt,
                    None => {
                        // Wrong PIN / tampered — drop it.
                        shared.dropped_encrypted.fetch_add(1, Ordering::Relaxed);
                        continue;
                    }
                }
            } else {
                payload.to_vec()
            };

            if pcm_bytes.len() % 2 != 0 {
                continue;
            }
            let mut peak: i16 = 0;
            let mut samples = Vec::with_capacity(pcm_bytes.len() / 2);
            for pair in pcm_bytes.chunks_exact(2) {
                let s = i16::from_le_bytes([pair[0], pair[1]]);
                let a = s.saturating_abs();
                if a > peak {
                    peak = a;
                }
                samples.push(s);
            }

            if shared.noise_suppression.load(Ordering::Relaxed) {
                denoised.clear();
                denoiser.process(&samples, &mut denoised);
                sink.push(&denoised);
            } else {
                sink.push(&samples);
            }

            shared.set_peak(peak as f32 / 32768.0);
            shared.packets.fetch_add(1, Ordering::Relaxed);
            shared.last_packet_ms.store(now_ms(), Ordering::Relaxed);
            *shared.peer.lock().unwrap() = peer.ip().to_string();
        }
    });
}

struct App {
    shared: Arc<Shared>,
    ip: String,
    pin: String,
    level: f32, // smoothed meter value
    setup_status: Arc<Mutex<Option<String>>>,
}

impl App {
    fn new() -> Self {
        let shared = Arc::new(Shared::default());
        spawn_receiver(shared.clone());
        App {
            shared,
            ip: local_ip(),
            pin: String::new(),
            level: 0.0,
            setup_status: Arc::new(Mutex::new(None)),
        }
    }
}

/// Download VB-CABLE and launch its (elevated) installer, so the whole
/// "make my phone a real mic" setup happens from inside the app. VB-CABLE's
/// installer still requires one click ("Install Driver") and a reboot — that
/// part can't be silenced — but everything up to it is automated. Uses
/// PowerShell so we add no HTTP/zip/elevation dependencies (minimal bloat).
fn install_vbcable(status: Arc<Mutex<Option<String>>>) {
    *status.lock().unwrap() = Some("Downloading VB-CABLE…".to_string());
    std::thread::spawn(move || {
        let script = r#"
$ErrorActionPreference='Stop'
$u='https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip'
$d=Join-Path $env:TEMP 'phonemic_vbcable'
New-Item -ItemType Directory -Force $d | Out-Null
$z=Join-Path $d 'vbcable.zip'
Invoke-WebRequest -Uri $u -OutFile $z
Expand-Archive -Path $z -DestinationPath $d -Force
$exe=Join-Path $d 'VBCABLE_Setup_x64.exe'
Start-Process -FilePath $exe -Verb RunAs
"#;
        let out = std::process::Command::new("powershell")
            .args(["-NoProfile", "-Command", script])
            .output();
        let msg = match out {
            Ok(o) if o.status.success() => {
                "Installer opened. Click “Install Driver”, then REBOOT and reopen PhoneMic.".to_string()
            }
            Ok(o) => format!(
                "Setup failed: {}",
                String::from_utf8_lossy(&o.stderr)
                    .lines()
                    .find(|l| !l.trim().is_empty())
                    .unwrap_or("check your internet connection")
            ),
            Err(e) => format!("Couldn't launch setup: {e}"),
        };
        *status.lock().unwrap() = Some(msg);
    });
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Smooth the meter: fast attack, slow release.
        self.level = (self.level * 0.82).max(self.shared.peak());
        let connected = self.shared.packets.load(Ordering::Relaxed) > 0
            && now_ms().saturating_sub(self.shared.last_packet_ms.load(Ordering::Relaxed)) < 1500;

        egui::CentralPanel::default()
            .frame(egui::Frame::none().fill(BG).inner_margin(egui::Margin::same(20.0)))
            .show(ctx, |ui| {
                // ---- Header ----
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new("🎙").size(26.0));
                    ui.add_space(2.0);
                    ui.vertical(|ui| {
                        ui.label(egui::RichText::new("PhoneMic").size(22.0).strong().color(TEXT));
                        ui.label(egui::RichText::new("your phone, as a microphone").size(12.0).color(MUTED));
                    });
                });
                ui.add_space(16.0);

                if let Some(err) = self.shared.error.lock().unwrap().clone() {
                    card(ui, |ui| {
                        ui.colored_label(egui::Color32::from_rgb(0xE0, 0x4B, 0x4B), format!("⚠  {err}"));
                    });
                    ctx.request_repaint_after(Duration::from_millis(300));
                    return;
                }

                // ---- Connection card ----
                card(ui, |ui| {
                    ui.label(egui::RichText::new("CONNECT YOUR PHONE").size(11.0).color(MUTED).strong());
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        ui.label(egui::RichText::new(&self.ip).size(30.0).strong().color(ACCENT).monospace());
                        ui.add_space(6.0);
                        if ui.button("Copy").clicked() {
                            ui.ctx().copy_text(self.ip.clone());
                        }
                    });
                    ui.label(egui::RichText::new(format!("port {PORT}  •  enter this in the app, tap Start")).size(12.0).color(MUTED));

                    ui.add_space(10.0);
                    ui.horizontal(|ui| {
                        ui.label(egui::RichText::new("🔒").size(14.0));
                        let resp = ui.add(
                            egui::TextEdit::singleline(&mut self.pin)
                                .hint_text("Security PIN (optional)")
                                .desired_width(170.0),
                        );
                        if resp.changed() {
                            *self.shared.pin.lock().unwrap() = self.pin.trim().to_string();
                        }
                    });
                    ui.label(egui::RichText::new("Set the same PIN here and in the app to encrypt the audio.").size(11.0).color(MUTED));

                    ui.add_space(12.0);

                    // status pill
                    let (dot, txt) = if connected {
                        (GREEN, format!("Connected — {}", self.shared.peer.lock().unwrap()))
                    } else {
                        (MUTED, "Waiting for phone…".to_string())
                    };
                    ui.horizontal(|ui| {
                        ui.label(egui::RichText::new("●").color(dot).size(14.0));
                        ui.label(egui::RichText::new(txt).color(if connected { TEXT } else { MUTED }));
                    });
                });

                ui.add_space(14.0);

                // ---- Level meter ----
                card(ui, |ui| {
                    ui.label(egui::RichText::new("MIC LEVEL").size(11.0).color(MUTED).strong());
                    ui.add_space(8.0);
                    ui.add(
                        egui::ProgressBar::new(self.level.clamp(0.0, 1.0))
                            .desired_width(f32::INFINITY)
                            .desired_height(16.0)
                            .fill(if self.level > 0.9 { egui::Color32::from_rgb(0xE0, 0x8A, 0x2B) } else { ACCENT })
                            .rounding(8.0),
                    );
                });

                ui.add_space(14.0);

                // ---- Voice / audio settings ----
                card(ui, |ui| {
                    ui.label(egui::RichText::new("VOICE").size(11.0).color(MUTED).strong());
                    ui.add_space(8.0);
                    let mut ns = self.shared.noise_suppression.load(Ordering::Relaxed);
                    ui.horizontal(|ui| {
                        if ui.add(egui::Checkbox::without_text(&mut ns)).changed() {
                            self.shared.noise_suppression.store(ns, Ordering::Relaxed);
                        }
                        ui.vertical(|ui| {
                            ui.label(egui::RichText::new("Voice Focus").strong().color(TEXT));
                            ui.label(egui::RichText::new("Isolate your voice — removes fans, hiss, keyboard, room noise").size(12.0).color(MUTED));
                        });
                    });
                });

                ui.add_space(14.0);

                // ---- Output routing ----
                card(ui, |ui| {
                    ui.label(egui::RichText::new("OUTPUT").size(11.0).color(MUTED).strong());
                    ui.add_space(8.0);
                    let device = self.shared.device.lock().unwrap().clone();
                    if self.shared.is_cable.load(Ordering::Relaxed) {
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new("✓").color(GREEN).strong());
                            ui.label(egui::RichText::new("Microphone mode").color(TEXT).strong());
                        });
                        ui.label(egui::RichText::new(format!("routing to {device}")).size(12.0).color(MUTED));
                        ui.label(egui::RichText::new("In Discord / Zoom, choose mic: “CABLE Output”").size(12.0).color(TEXT));
                    } else {
                        ui.label(egui::RichText::new("Speaker test mode").color(TEXT).strong());
                        ui.label(egui::RichText::new(format!("playing to {device}")).size(12.0).color(MUTED));
                        ui.label(egui::RichText::new("To use your phone as a mic in apps, set up the virtual microphone:").size(12.0).color(MUTED));
                        ui.add_space(8.0);
                        let status = self.setup_status.lock().unwrap().clone();
                        let busy = status.as_deref() == Some("Downloading VB-CABLE…");
                        if ui
                            .add_enabled(
                                !busy,
                                egui::Button::new(egui::RichText::new("⚙  Set up microphone").strong().color(TEXT))
                                    .fill(ACCENT),
                            )
                            .clicked()
                        {
                            install_vbcable(self.setup_status.clone());
                        }
                        if let Some(msg) = status {
                            ui.add_space(6.0);
                            ui.label(egui::RichText::new(msg).size(12.0).color(if busy { MUTED } else { GREEN }));
                        }
                    }
                });

                // ---- Footer ----
                ui.add_space(10.0);
                ui.with_layout(egui::Layout::bottom_up(egui::Align::LEFT), |ui| {
                    ui.label(egui::RichText::new(format!("{} packets", self.shared.packets.load(Ordering::Relaxed))).size(11.0).color(MUTED));
                });
            });

        // Keep the meter/status live.
        ctx.request_repaint_after(Duration::from_millis(33));
    }
}

// Palette (dark, calm, one accent).
const BG: egui::Color32 = egui::Color32::from_rgb(0x14, 0x16, 0x1B);
const CARD: egui::Color32 = egui::Color32::from_rgb(0x1E, 0x21, 0x29);
const ACCENT: egui::Color32 = egui::Color32::from_rgb(0x4C, 0x8B, 0xFF);
const GREEN: egui::Color32 = egui::Color32::from_rgb(0x37, 0xD0, 0x6A);
const MUTED: egui::Color32 = egui::Color32::from_rgb(0x8A, 0x90, 0x9C);
const TEXT: egui::Color32 = egui::Color32::from_rgb(0xE9, 0xEC, 0xF2);

fn setup_style(ctx: &egui::Context) {
    let mut v = egui::Visuals::dark();
    v.override_text_color = Some(TEXT);
    v.panel_fill = BG;
    v.window_fill = BG;
    v.faint_bg_color = CARD;
    v.extreme_bg_color = egui::Color32::from_rgb(0x0F, 0x11, 0x15);
    v.selection.bg_fill = ACCENT.gamma_multiply(0.5);
    v.widgets.inactive.bg_fill = egui::Color32::from_rgb(0x2A, 0x2E, 0x38);
    v.widgets.hovered.bg_fill = egui::Color32::from_rgb(0x33, 0x38, 0x45);
    v.widgets.active.bg_fill = ACCENT;
    v.hyperlink_color = ACCENT;
    let round = egui::Rounding::same(10.0);
    v.widgets.inactive.rounding = round;
    v.widgets.hovered.rounding = round;
    v.widgets.active.rounding = round;
    ctx.set_visuals(v);

    let mut style = (*ctx.style()).clone();
    style.spacing.item_spacing = egui::vec2(10.0, 10.0);
    style.spacing.button_padding = egui::vec2(14.0, 8.0);
    ctx.set_style(style);
}

/// A rounded card container.
fn card<R>(ui: &mut egui::Ui, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    egui::Frame::none()
        .fill(CARD)
        .rounding(14.0)
        .inner_margin(egui::Margin::same(16.0))
        .show(ui, add)
        .inner
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([440.0, 600.0])
            .with_min_inner_size([400.0, 560.0])
            .with_resizable(true),
        ..Default::default()
    };
    eframe::run_native(
        "PhoneMic",
        options,
        Box::new(|cc| {
            setup_style(&cc.egui_ctx);
            Ok(Box::new(App::new()))
        }),
    )
}
