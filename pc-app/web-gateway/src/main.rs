//! PhoneMic web gateway.
//!
//! Serves the browser web client over HTTPS and accepts its WebSocket stream of
//! PCM16 packets (the same `phonemic-protocol` framing the native app uses),
//! decodes them, and plays them out the PC's default output device. This is the
//! web equivalent of the Phase 0 native receiver: prove the loop is audible.
//!
//! HTTPS is mandatory because browsers only expose `getUserMedia` in a secure
//! context; we mint a self-signed cert at startup (the user accepts the warning
//! once). Real network audio arrives over `wss://…/ws`.

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use axum::response::IntoResponse;
use axum::routing::get;
use axum::Router;
use axum_server::tls_rustls::RustlsConfig;
use tokio::sync::Mutex;
use tower_http::services::ServeDir;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, SizedSample};
use ringbuf::traits::{Consumer, Producer, Split};
use ringbuf::{HeapCons, HeapProd, HeapRb};

use phonemic_core::jitter_buffer::{JitterBuffer, Pop};
use phonemic_protocol::{decode, pcm16_sample_count, Codec};

mod driver_feed;

/// Where decoded PCM goes: the speaker ring (dev) or the driver's shared ring.
trait PcmSink: Send {
    fn push_samples(&mut self, samples: &[i16]);
}
impl PcmSink for HeapProd<i16> {
    fn push_samples(&mut self, samples: &[i16]) {
        self.push_slice(samples);
    }
}
impl PcmSink for driver_feed::DriverFeed {
    fn push_samples(&mut self, samples: &[i16]) {
        self.feed(samples);
    }
}

const SAMPLE_RATE: u32 = 48_000;
const FRAME_SAMPLES: usize = 480; // 10 ms @ 48 kHz
/// Jitter-buffer sizing: hold ~30 ms (3 frames) to absorb network jitter,
/// with room to reorder up to ~320 ms of packets.
const JB_CAPACITY: u32 = 32;
const JB_TARGET_DEPTH: u32 = 3;

#[derive(Clone)]
struct AppState {
    /// Reorder + loss-concealment buffer that the WS handler fills by sequence
    /// number and the playout timer drains. Shared; one browser at a time.
    jitter: Arc<Mutex<JitterBuffer>>,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // args: [bind_host] [https_port] [web_dir]
    let mut args = std::env::args().skip(1);
    let host = args.next().unwrap_or_else(|| "0.0.0.0".to_string());
    let port: u16 = args
        .next()
        .and_then(|s| s.parse().ok())
        .unwrap_or(8443);
    let web_dir = args.next().unwrap_or_else(|| "web-client".to_string());

    // Choose where audio goes: the PhoneMic driver's shared ring (--driver) or
    // the local speakers (default, dev mode).
    let driver_mode = std::env::args().any(|a| a == "--driver");
    let sink: Box<dyn PcmSink> = if driver_mode {
        println!("  audio → shared ring for the PhoneMic driver (Global\\PhonemicRing)");
        Box::new(driver_feed::DriverFeed::create()?)
    } else {
        println!("  audio → default output @ {SAMPLE_RATE} Hz");
        // cpal Stream is !Send on Windows, so it lives on its own thread; we only
        // move the ring producer here.
        Box::new(spawn_audio_output()?)
    };

    // Playout pipeline: the WS handler inserts packets into the jitter buffer by
    // sequence number; this timer drains one frame every 10 ms, concealing gaps
    // with silence, and feeds the chosen sink. This is where reorder tolerance
    // and loss concealment actually happen.
    let jitter = Arc::new(Mutex::new(JitterBuffer::new(JB_CAPACITY, JB_TARGET_DEPTH)));
    spawn_playout(jitter.clone(), sink);
    let state = AppState { jitter };

    // Self-signed cert covering localhost plus whatever host the user browses.
    let san = vec!["localhost".to_string(), host.clone()];
    let cert = rcgen::generate_simple_self_signed(san)?;
    let tls = RustlsConfig::from_pem(
        cert.cert.pem().into_bytes(),
        cert.key_pair.serialize_pem().into_bytes(),
    )
    .await?;

    let app = Router::new()
        .route("/ws", get(ws_handler))
        .fallback_service(ServeDir::new(&web_dir))
        .with_state(state);

    let addr: SocketAddr = format!("{host}:{port}").parse()?;
    println!("PhoneMic web gateway");
    println!("  serving {web_dir}/ + wss on https://{addr}");
    println!("  open  https://<this-pc-lan-ip>:{port}/  on your phone (accept the cert warning)");

    axum_server::bind_rustls(addr, tls)
        .serve(app.into_make_service())
        .await?;
    Ok(())
}

async fn ws_handler(ws: WebSocketUpgrade, State(state): State<AppState>) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, state))
}

/// One browser connection: receive framed PCM16 packets and hand them to the
/// jitter buffer by sequence number. Playout/concealment happens in the timer.
async fn handle_socket(mut socket: WebSocket, state: AppState) {
    println!("browser connected");
    let mut received: u64 = 0;

    while let Some(Ok(msg)) = socket.recv().await {
        let Message::Binary(buf) = msg else { continue };
        let (header, payload) = match decode(&buf) {
            Ok(v) => v,
            Err(_) => continue, // malformed frame; drop it (loss tolerance)
        };

        // Only PCM16 with a whole number of samples is playable today.
        if header.codec == Codec::Pcm16 && pcm16_sample_count(payload).is_some() {
            state
                .jitter
                .lock()
                .await
                .insert(header.seq, payload.to_vec());
            received += 1;
        }
    }
    println!("browser disconnected ({received} frames)");
}

/// Drain the jitter buffer at the frame cadence and feed the chosen sink.
fn spawn_playout(jitter: Arc<Mutex<JitterBuffer>>, mut sink: Box<dyn PcmSink>) {
    tokio::spawn(async move {
        let silence = [0i16; FRAME_SAMPLES];
        let mut tick = tokio::time::interval(Duration::from_millis(10));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            tick.tick().await;
            let popped = { jitter.lock().await.pop() };
            match popped {
                Pop::Frame(bytes) => {
                    // bytes are little-endian PCM16; convert and enqueue.
                    let mut samples = Vec::with_capacity(bytes.len() / 2);
                    for pair in bytes.chunks_exact(2) {
                        samples.push(i16::from_le_bytes([pair[0], pair[1]]));
                    }
                    sink.push_samples(&samples);
                }
                // Lost packet → emit a frame of silence so the timeline holds.
                // (Opus PLC replaces this once Opus decoding lands.)
                Pop::Conceal => {
                    sink.push_samples(&silence);
                }
                // Still priming or underrun → device plays its own silence.
                Pop::Empty => {}
            }
        }
    });
}

// --- Audio output ------------------------------------------------------------

/// Start the default output device on a dedicated thread and return the ring
/// producer to push mono PCM16 into.
fn spawn_audio_output() -> Result<HeapProd<i16>, Box<dyn std::error::Error>> {
    let rb = HeapRb::<i16>::new(SAMPLE_RATE as usize); // ~1 s
    let (producer, consumer) = rb.split();

    // Probe the device on this thread so errors surface at startup, then move
    // the (non-Send) stream onto its own thread and keep it alive there.
    let (tx, rx) = std::sync::mpsc::channel::<Result<(), String>>();
    std::thread::spawn(move || match run_output(consumer) {
        Ok(stream) => {
            let _ = tx.send(Ok(()));
            // Keep the stream alive for the process lifetime.
            std::thread::park();
            drop(stream);
        }
        Err(e) => {
            let _ = tx.send(Err(e.to_string()));
        }
    });

    match rx.recv() {
        Ok(Ok(())) => Ok(producer),
        Ok(Err(e)) => Err(e.into()),
        Err(_) => Err("audio output thread died during startup".into()),
    }
}

fn run_output(consumer: HeapCons<i16>) -> Result<cpal::Stream, Box<dyn std::error::Error>> {
    let host = cpal::default_host();
    let device = host
        .default_output_device()
        .ok_or("no default output device")?;
    let default_cfg = device.default_output_config()?;
    let sample_format = default_cfg.sample_format();
    let channels = default_cfg.channels();
    let config = cpal::StreamConfig {
        channels,
        sample_rate: cpal::SampleRate(SAMPLE_RATE),
        buffer_size: cpal::BufferSize::Default,
    };

    let stream = match sample_format {
        cpal::SampleFormat::F32 => build_stream::<f32>(&device, &config, consumer)?,
        cpal::SampleFormat::I16 => build_stream::<i16>(&device, &config, consumer)?,
        cpal::SampleFormat::U16 => build_stream::<u16>(&device, &config, consumer)?,
        other => return Err(format!("unsupported sample format: {other:?}").into()),
    };
    stream.play()?;
    Ok(stream)
}

fn build_stream<T>(
    device: &cpal::Device,
    config: &cpal::StreamConfig,
    mut consumer: HeapCons<i16>,
) -> Result<cpal::Stream, Box<dyn std::error::Error>>
where
    T: SizedSample + FromSample<i16>,
{
    let channels = config.channels as usize;
    let stream = device.build_output_stream(
        config,
        move |data: &mut [T], _: &cpal::OutputCallbackInfo| {
            for frame in data.chunks_mut(channels) {
                let mono = consumer.try_pop().unwrap_or(0);
                let s = T::from_sample(mono);
                for out in frame.iter_mut() {
                    *out = s;
                }
            }
        },
        move |err| eprintln!("audio output error: {err}"),
        None,
    )?;
    Ok(stream)
}
