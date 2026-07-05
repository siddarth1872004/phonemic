//! Dev-mode audio playback sink (Phase 0 only).
//!
//! Plays received PCM out the PC's default output device via `cpal`, so we can
//! literally hear the phone→PC loop before any driver exists. In Phase 4 this
//! whole module is replaced by the shared-memory IPC to the virtual driver; the
//! rest of the core does not care which sink it feeds.
//!
//! Threading: the network thread produces samples, the `cpal` realtime callback
//! consumes them. The handoff is a lock-free SPSC ring buffer (`ringbuf`) so the
//! audio thread never blocks — see principle #2 (low latency) in the brief.

use std::error::Error;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, SizedSample};
use ringbuf::traits::{Consumer, Producer, Split};
use ringbuf::{HeapCons, HeapProd, HeapRb};

/// The stream sample rate we run at, matching the phone's capture rate. Keeping
/// both ends at 48 kHz avoids any resampling on the hot path in Phase 0.
pub const SAMPLE_RATE: u32 = 48_000;

/// Ring-buffer capacity in mono samples (~1 s at 48 kHz). Generous for Phase 0;
/// the real, tight jitter buffer arrives in Phase 1.
const RING_CAPACITY: usize = SAMPLE_RATE as usize;

/// Owns the output stream and the producer half of the sample ring.
pub struct AudioSink {
    producer: HeapProd<i16>,
    // The stream must stay alive for audio to keep flowing; dropping it stops
    // playback. Underscore-prefixed because we never touch it again after start.
    _stream: cpal::Stream,
    /// Output device sample rate actually in use.
    pub sample_rate: u32,
    /// Output device channel count (mono input is duplicated across these).
    pub channels: u16,
    /// Name of the output device we ended up on (for a friendly banner).
    pub device_name: String,
    /// True if we routed into a virtual cable (VB-CABLE) rather than speakers.
    pub is_virtual_cable: bool,
}

impl AudioSink {
    /// Open an output device and start playing whatever gets pushed.
    ///
    /// If `prefer` is set, pick the first output device whose name contains that
    /// substring (case-insensitive) — used to auto-route into "CABLE Input"
    /// (VB-CABLE) so the phone shows up as a real microphone. Falls back to the
    /// default output device (speakers) when no match is found.
    pub fn new(prefer: Option<&str>) -> Result<Self, Box<dyn Error>> {
        let host = cpal::default_host();

        let mut matched = false;
        let device = match prefer {
            Some(sub) => {
                let want = sub.to_lowercase();
                let found = host.output_devices().ok().and_then(|mut devs| {
                    devs.find(|d| {
                        d.name()
                            .map(|n| n.to_lowercase().contains(&want))
                            .unwrap_or(false)
                    })
                });
                match found {
                    Some(d) => {
                        matched = true;
                        d
                    }
                    None => host
                        .default_output_device()
                        .ok_or("no output device found")?,
                }
            }
            None => host
                .default_output_device()
                .ok_or("no output device found")?,
        };

        let device_name = device.name().unwrap_or_else(|_| "unknown".to_string());
        let default_cfg = device.default_output_config()?;
        let sample_format = default_cfg.sample_format();
        let channels = default_cfg.channels();

        // Force 48 kHz to match the phone; keep the device's channel count.
        let config = cpal::StreamConfig {
            channels,
            sample_rate: cpal::SampleRate(SAMPLE_RATE),
            buffer_size: cpal::BufferSize::Default,
        };

        let rb = HeapRb::<i16>::new(RING_CAPACITY);
        let (producer, consumer) = rb.split();

        let stream = match sample_format {
            cpal::SampleFormat::F32 => build_stream::<f32>(&device, &config, consumer),
            cpal::SampleFormat::I16 => build_stream::<i16>(&device, &config, consumer),
            cpal::SampleFormat::U16 => build_stream::<u16>(&device, &config, consumer),
            other => Err(format!("unsupported output sample format: {other:?}").into()),
        }?;

        stream.play()?;

        Ok(Self {
            producer,
            _stream: stream,
            sample_rate: SAMPLE_RATE,
            channels,
            device_name,
            is_virtual_cable: matched,
        })
    }

    /// Push mono PCM16 samples for playback. Returns how many were accepted;
    /// fewer than `samples.len()` means the ring overran (we're receiving faster
    /// than the device drains) and the excess was dropped.
    pub fn push(&mut self, samples: &[i16]) -> usize {
        self.producer.push_slice(samples)
    }
}

/// Build a typed output stream. Each output frame pulls one mono sample and
/// duplicates it across all channels; an empty ring yields silence (underrun).
fn build_stream<T>(
    device: &cpal::Device,
    config: &cpal::StreamConfig,
    mut consumer: HeapCons<i16>,
) -> Result<cpal::Stream, Box<dyn Error>>
where
    T: SizedSample + FromSample<i16>,
{
    let channels = config.channels as usize;
    let stream = device.build_output_stream(
        config,
        move |data: &mut [T], _: &cpal::OutputCallbackInfo| {
            for frame in data.chunks_mut(channels) {
                let mono = consumer.try_pop().unwrap_or(0);
                let sample = T::from_sample(mono);
                for out in frame.iter_mut() {
                    *out = sample;
                }
            }
        },
        move |err| eprintln!("audio output stream error: {err}"),
        None,
    )?;
    Ok(stream)
}
