//! PhoneMic PC receiver — Phase 0 proof of life.
//!
//! Binds a UDP socket, receives PCM16 packets from the phone, and plays them out
//! the default output device so you can hear the phone→PC loop. No Opus, no
//! jitter buffer, no driver yet — those are Phases 1 and 4. This binary exists
//! purely to prove sound flows end to end.

use std::net::SocketAddr;

use phonemic_protocol::{decode, pcm16_sample_count, Codec, ProtocolError};

use phonemic_core::sink::AudioSink;
use phonemic_core::transport::{Transport, UdpTransport};

/// Default port the phone streams to. Override with `PHONEMIC_PORT`.
const DEFAULT_PORT: u16 = 4010;

/// Max UDP datagram we'll accept. 20 ms of mono 48 kHz PCM16 is 1920 bytes;
/// 2 KiB leaves comfortable headroom for header + larger frames.
const RECV_BUF_LEN: usize = 2048;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let port = std::env::var("PHONEMIC_PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(DEFAULT_PORT);

    let bind_addr: SocketAddr = ([0, 0, 0, 0], port).into();
    let transport = UdpTransport::bind(bind_addr)?;

    let mut sink = AudioSink::new()?;

    println!("PhoneMic receiver (Phase 0)");
    println!("  listening on   udp {}", transport.local_addr()?);
    println!(
        "  playing out    default output @ {} Hz, {} ch",
        sink.sample_rate, sink.channels
    );
    println!("  point the phone app at this PC's LAN IP, port {port}");
    println!("  Ctrl-C to stop.\n");

    let mut buf = [0u8; RECV_BUF_LEN];

    // Loss/quality accounting for Phase 0. Real concealment is Phase 1 (Opus).
    let mut expected_seq: Option<u32> = None;
    let mut received: u64 = 0;
    let mut lost: u64 = 0;
    let mut malformed: u64 = 0;
    let mut peer_logged = false;

    loop {
        let (n, peer) = match transport.recv(&mut buf) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("recv error: {e}");
                continue;
            }
        };

        if !peer_logged {
            println!("first packet from {peer}");
            peer_logged = true;
        }

        let (header, payload) = match decode(&buf[..n]) {
            Ok(v) => v,
            Err(e) => {
                malformed += 1;
                log_decode_error(&e, &mut malformed);
                continue;
            }
        };

        received += 1;
        account_for_loss(header.seq, &mut expected_seq, &mut lost);

        match header.codec {
            Codec::Pcm16 => play_pcm16(payload, &mut sink),
            // Opus arrives in Phase 1; for now we simply skip it rather than
            // guess. Skipping keeps Phase 0 honest about what it supports.
            Codec::Opus => { /* not yet decoded */ }
        }

        if received % 500 == 0 {
            let total = received + lost;
            let loss_pct = if total > 0 {
                (lost as f64 / total as f64) * 100.0
            } else {
                0.0
            };
            println!(
                "packets: {received} ok, {lost} lost ({loss_pct:.2}%), {malformed} malformed"
            );
        }
    }
}

/// Split a PCM16 payload into `i16` samples and hand them to the sink.
fn play_pcm16(payload: &[u8], sink: &mut AudioSink) {
    let Some(count) = pcm16_sample_count(payload) else {
        // Odd byte count => a partial sample => corruption. Drop it.
        return;
    };
    let mut samples = Vec::with_capacity(count);
    for pair in payload.chunks_exact(2) {
        samples.push(i16::from_le_bytes([pair[0], pair[1]]));
    }
    let accepted = sink.push(&samples);
    if accepted < samples.len() {
        // Ring overrun: receiving faster than the device drains. Rare in Phase 0.
        eprintln!(
            "sink overrun: dropped {} samples",
            samples.len() - accepted
        );
    }
}

/// Update loss statistics from a newly received sequence number.
fn account_for_loss(seq: u32, expected: &mut Option<u32>, lost: &mut u64) {
    if let Some(exp) = *expected {
        // Only count forward gaps as loss; reordered/duplicate packets that
        // arrive "behind" are ignored here (the Phase 1 jitter buffer handles
        // reordering properly).
        if seq > exp {
            *lost += (seq - exp) as u64;
        }
    }
    *expected = Some(seq.wrapping_add(1));
}

/// Log decode errors at a low rate so a hostile/noisy port can't flood stderr.
fn log_decode_error(err: &ProtocolError, malformed: &mut u64) {
    if *malformed <= 5 || *malformed % 1000 == 0 {
        eprintln!("dropped malformed packet ({malformed}): {err:?}");
    }
}
