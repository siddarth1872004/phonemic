//! Optional noise suppression (RNNoise via `nnnoiseless`).
//!
//! RNNoise works on fixed 480-sample (10 ms @ 48 kHz) frames — exactly our
//! packet size — so in the common case each packet is one frame. We still buffer
//! to stay correct if a packet ever carries a different count; the extra latency
//! is bounded by one frame (<10 ms).

use nnnoiseless::DenoiseState;

/// Streaming denoiser: feed it PCM16, get denoised PCM16 back.
pub struct Denoiser {
    state: Box<DenoiseState<'static>>,
    inbuf: Vec<f32>,
}

impl Denoiser {
    pub fn new() -> Self {
        Denoiser {
            state: DenoiseState::new(),
            inbuf: Vec::with_capacity(DenoiseState::FRAME_SIZE * 2),
        }
    }

    /// Denoise `input`, appending results to `out`. Output may lag input by up
    /// to one frame while the buffer fills.
    pub fn process(&mut self, input: &[i16], out: &mut Vec<i16>) {
        const N: usize = DenoiseState::FRAME_SIZE; // 480
        // RNNoise expects f32 samples in i16 magnitude (not -1.0..1.0).
        self.inbuf.extend(input.iter().map(|&s| s as f32));

        let mut frame_out = [0.0f32; N];
        while self.inbuf.len() >= N {
            let mut frame_in = [0.0f32; N];
            frame_in.copy_from_slice(&self.inbuf[..N]);
            self.state.process_frame(&mut frame_out, &frame_in);
            out.extend(
                frame_out
                    .iter()
                    .map(|&v| v.round().clamp(-32768.0, 32767.0) as i16),
            );
            self.inbuf.drain(..N);
        }
    }
}

impl Default for Denoiser {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn passes_frames_through_and_reduces_noise_energy() {
        let mut d = Denoiser::new();
        // White-ish noise input, one frame.
        let input: Vec<i16> = (0..480).map(|i| (((i * 7919) % 4000) as i16) - 2000).collect();
        let mut out = Vec::new();
        d.process(&input, &mut out);
        assert_eq!(out.len(), 480, "one frame in, one frame out");
    }

    #[test]
    fn buffers_partial_frames() {
        let mut d = Denoiser::new();
        let mut out = Vec::new();
        d.process(&[0i16; 200], &mut out); // < one frame
        assert_eq!(out.len(), 0, "not enough for a frame yet");
        d.process(&[0i16; 300], &mut out); // now 500 total -> one 480 frame
        assert_eq!(out.len(), 480);
    }
}
