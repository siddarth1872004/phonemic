//! "Software phone" — emulates the Android sender on the PC.
//!
//! Streams synthetic 48 kHz mono PCM16 to the receiver using the *real* wire
//! protocol (`phonemic-protocol`), stamping each packet with a wall-clock
//! (`SystemTime`) timestamp so the receiver's `--measure` mode can compute
//! transport latency. This lets the whole PC pipeline be exercised and measured
//! without an actual Android device — it is a test/dev tool, not a shipped
//! component.
//!
//! Usage: `phonemic-softphone [target_ip] [port]`  (defaults 127.0.0.1 4010)

use std::net::UdpSocket;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use phonemic_protocol::{encode, Codec};

const SAMPLE_RATE: u32 = 48_000;
const FRAME_MS: u32 = 10;
const SAMPLES_PER_FRAME: usize = (SAMPLE_RATE as usize * FRAME_MS as usize) / 1000; // 480

/// Wall-clock microseconds since the Unix epoch. Comparable across processes on
/// the same machine, which is how the receiver measures one-way latency.
fn now_micros() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_micros() as u64
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let target_ip = args.next().unwrap_or_else(|| "127.0.0.1".to_string());
    let port: u16 = args
        .next()
        .and_then(|s| s.parse().ok())
        .or_else(|| std::env::var("PHONEMIC_PORT").ok().and_then(|s| s.parse().ok()))
        .unwrap_or(4010);
    let dest = format!("{target_ip}:{port}");

    let sock = UdpSocket::bind("0.0.0.0:0")?;
    sock.connect(&dest)?;
    println!(
        "softphone → {dest}  ({SAMPLE_RATE} Hz mono, {FRAME_MS} ms frames, synth 440 Hz)"
    );

    let mut seq: u32 = 0;
    let mut phase: f32 = 0.0;
    let step = std::f32::consts::TAU * 440.0 / SAMPLE_RATE as f32;
    let mut pcm = vec![0u8; SAMPLES_PER_FRAME * 2];
    let mut pkt = vec![0u8; 2048];

    loop {
        for i in 0..SAMPLES_PER_FRAME {
            let sample = (phase.sin() * 8000.0) as i16;
            phase += step;
            if phase > std::f32::consts::TAU {
                phase -= std::f32::consts::TAU;
            }
            let b = sample.to_le_bytes();
            pcm[i * 2] = b[0];
            pcm[i * 2 + 1] = b[1];
        }
        // Fixed sizes here, so encode cannot fail; treat any error as a bug.
        let n = encode(Codec::Pcm16, seq, now_micros(), &pcm, &mut pkt)
            .expect("softphone frame always fits the packet buffer");
        sock.send(&pkt[..n])?;
        seq = seq.wrapping_add(1);
        std::thread::sleep(Duration::from_millis(FRAME_MS as u64));
    }
}
