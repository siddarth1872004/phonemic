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

use phonemic_protocol::{decode, pcm16_sample_count, Codec};

const SAMPLE_RATE: u32 = 48_000;

#[derive(Clone)]
struct AppState {
    /// Producer half of the ring feeding the audio output thread. A single
    /// browser connection at a time is expected; the Mutex serialises pushes.
    audio: Arc<Mutex<HeapProd<i16>>>,
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

    // Audio output on its own thread (cpal Stream is !Send on Windows).
    let producer = spawn_audio_output()?;
    let state = AppState {
        audio: Arc::new(Mutex::new(producer)),
    };

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
    println!("  audio → default output @ {SAMPLE_RATE} Hz");

    axum_server::bind_rustls(addr, tls)
        .serve(app.into_make_service())
        .await?;
    Ok(())
}

async fn ws_handler(ws: WebSocketUpgrade, State(state): State<AppState>) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, state))
}

/// One browser connection: receive framed PCM16 packets and enqueue samples.
async fn handle_socket(mut socket: WebSocket, state: AppState) {
    println!("browser connected");
    let mut received: u64 = 0;
    let mut expected_seq: Option<u32> = None;
    let mut lost: u64 = 0;

    while let Some(Ok(msg)) = socket.recv().await {
        let Message::Binary(buf) = msg else { continue };
        let (header, payload) = match decode(&buf) {
            Ok(v) => v,
            Err(_) => continue, // malformed frame; drop it (loss tolerance)
        };

        if let Some(exp) = expected_seq {
            if header.seq > exp {
                lost += (header.seq - exp) as u64;
            }
        }
        expected_seq = Some(header.seq.wrapping_add(1));

        if header.codec == Codec::Pcm16 {
            if let Some(count) = pcm16_sample_count(payload) {
                let mut samples = Vec::with_capacity(count);
                for pair in payload.chunks_exact(2) {
                    samples.push(i16::from_le_bytes([pair[0], pair[1]]));
                }
                let mut prod = state.audio.lock().await;
                prod.push_slice(&samples);
            }
        }

        received += 1;
        if received % 500 == 0 {
            println!("frames: {received} ok, {lost} lost");
        }
    }
    println!("browser disconnected ({received} frames)");
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
