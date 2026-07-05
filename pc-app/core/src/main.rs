//! PhoneMic PC receiver — Phase 0 proof of life.
//!
//! Binds a UDP socket, receives PCM16 packets from the phone, and plays them out
//! the default output device so you can hear the phone→PC loop. No Opus, no
//! jitter buffer, no driver yet — those are Phases 1 and 4. This binary exists
//! purely to prove sound flows end to end.

use std::net::SocketAddr;
use std::time::{SystemTime, UNIX_EPOCH};

use phonemic_protocol::{decode, pcm16_sample_count, Codec, ProtocolError};

use phonemic_core::sink::AudioSink;
use phonemic_core::transport::{Transport, UdpTransport};

/// Default port the phone streams to. Override with `PHONEMIC_PORT`.
const DEFAULT_PORT: u16 = 4010;

/// Max UDP datagram we'll accept. 20 ms of mono 48 kHz PCM16 is 1920 bytes;
/// 2 KiB leaves comfortable headroom for header + larger frames.
const RECV_BUF_LEN: usize = 2048;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let measure = std::env::args().any(|a| a == "--measure");
    let port = std::env::var("PHONEMIC_PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(DEFAULT_PORT);

    let bind_addr: SocketAddr = ([0, 0, 0, 0], port).into();
    let transport = UdpTransport::bind(bind_addr)?;

    if measure {
        // Latency harness: no audio device needed, collect a sample and report.
        return run_measure(&transport, port);
    }

    // Prefer VB-CABLE ("CABLE Input") so the phone becomes a real microphone
    // other apps can select; fall back to speakers if it isn't installed.
    let mut sink = AudioSink::new(Some("cable"))?;
    let lan_ip = local_ip().unwrap_or_else(|| "<your PC's IP>".to_string());

    println!("========================================================");
    println!("  PhoneMic  —  ready");
    println!("========================================================");
    println!();
    println!("  1) On your phone, in the PhoneMic app, enter:");
    println!("         IP:   {lan_ip}");
    println!("         port: {port}");
    println!("     then tap Start.");
    println!();
    if sink.is_virtual_cable {
        println!("  2) In Discord/Zoom/etc, pick this microphone:");
        println!("         \"CABLE Output (VB-Audio Virtual Cable)\"");
        println!("     (audio is routing into: {})", sink.device_name);
    } else {
        println!("  2) Heads up: VB-CABLE isn't installed, so right now the");
        println!("     phone audio just plays out your speakers:");
        println!("         {}", sink.device_name);
        println!("     To use it as a real mic, install VB-CABLE (free) and");
        println!("     restart this app — see START-HERE.md.");
    }
    println!();
    println!("  Leave this window open while you're using it. Ctrl-C to stop.");
    println!("--------------------------------------------------------\n");

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

/// Best-effort primary LAN IPv4 of this machine, with no dependencies: open a
/// UDP socket "toward" a public address (no packets are actually sent) and read
/// back which local interface the OS would route through.
fn local_ip() -> Option<String> {
    let sock = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
    sock.connect("8.8.8.8:80").ok()?;
    match sock.local_addr().ok()?.ip() {
        std::net::IpAddr::V4(v4) if !v4.is_loopback() => Some(v4.to_string()),
        _ => None,
    }
}

/// Wall-clock microseconds since the Unix epoch (see `softphone` for the pair).
fn now_micros() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_micros() as u64
}

/// `--measure` mode: collect a fixed number of packets and report one-way
/// latency (send-stamp → receive) statistics, then exit.
///
/// This measures the transport + decode + scheduling latency between the sender
/// and this process. Run against `phonemic-softphone` on loopback it isolates
/// the software/OS overhead (real Wi-Fi and the phone's capture latency are
/// additional, and the configured jitter-buffer depth adds `target_depth ×
/// frame_ms` on top). Sender and receiver compare the same OS wall clock, so on
/// one machine the numbers are directly comparable.
fn run_measure(
    transport: &UdpTransport,
    port: u16,
) -> Result<(), Box<dyn std::error::Error>> {
    const TARGET: usize = 300; // ~3 s of 10 ms frames

    println!("PhoneMic receiver (measure mode)");
    println!("  listening on udp {}", transport.local_addr()?);
    println!("  collecting {TARGET} packets on port {port}...\n");

    let mut buf = [0u8; RECV_BUF_LEN];
    let mut latencies_ms: Vec<f64> = Vec::with_capacity(TARGET);
    let mut expected_seq: Option<u32> = None;
    let mut lost: u64 = 0;
    let mut skipped_clock = 0u64;

    while latencies_ms.len() < TARGET {
        let (n, _) = transport.recv(&mut buf)?;
        let now = now_micros();
        let (header, _payload) = match decode(&buf[..n]) {
            Ok(v) => v,
            Err(_) => continue,
        };
        account_for_loss(header.seq, &mut expected_seq, &mut lost);
        // Guard against a stamp in the future (clock skew / different machine).
        if now >= header.timestamp_us {
            latencies_ms.push((now - header.timestamp_us) as f64 / 1000.0);
        } else {
            skipped_clock += 1;
        }
    }

    latencies_ms.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let count = latencies_ms.len();
    let pct = |p: f64| latencies_ms[((count - 1) as f64 * p) as usize];
    let mean = latencies_ms.iter().sum::<f64>() / count as f64;
    let jitter = {
        let var = latencies_ms.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / count as f64;
        var.sqrt()
    };

    println!("measured {count} packets ({lost} lost, {skipped_clock} skipped for clock skew)");
    println!(
        "one-way latency (ms):  min {:.3}   median {:.3}   p95 {:.3}   max {:.3}",
        latencies_ms[0],
        pct(0.50),
        pct(0.95),
        latencies_ms[count - 1]
    );
    println!("  mean {mean:.3} ms   jitter (stddev) {jitter:.3} ms");
    println!(
        "\nnote: this is software/transport latency only. Add the jitter-buffer\n\
         depth and, with a real phone, Oboe capture + Wi-Fi one-way time. Use the\n\
         acoustic clap test (docs/PHASE0-BRINGUP.md) for a true glass-to-glass number."
    );
    Ok(())
}
