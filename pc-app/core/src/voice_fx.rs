//! Real-time voice effects: pitch shifting and ring-mod "robot".
//!
//! The pitch shifter is the classic dual-tap crossfading delay line: two read
//! taps sweep through a short ring buffer at a rate offset from the write rate
//! (so the audio replays faster/slower = higher/lower pitch), and a triangular
//! crossfade hides each tap's jump back to the start of the window. Cheap,
//! dependency-free, low-latency (~one window, 40 ms), and plenty good for a
//! fun voice changer — this is not a studio formant-preserving shifter.

/// Which effect to apply. Kept as plain u8-mappable values so the GUI can pass
/// it through an atomic.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Effect {
    None,
    /// Pitch down ~5 semitones: big/deep voice.
    Deep,
    /// Pitch up ~6 semitones: chipmunk.
    Chipmunk,
    /// Ring modulation: classic robot/dalek.
    Robot,
}

impl Effect {
    pub fn from_u8(v: u8) -> Self {
        match v {
            1 => Effect::Deep,
            2 => Effect::Chipmunk,
            3 => Effect::Robot,
            _ => Effect::None,
        }
    }
    pub fn to_u8(self) -> u8 {
        match self {
            Effect::None => 0,
            Effect::Deep => 1,
            Effect::Chipmunk => 2,
            Effect::Robot => 3,
        }
    }
}

/// Window size in samples (~42 ms @ 48 kHz). Power of two for cheap wrapping.
const WINDOW: usize = 2048;
/// Ring capacity; must exceed WINDOW comfortably.
const RING: usize = WINDOW * 2;

pub struct VoiceFx {
    ring: [f32; RING],
    write: usize,
    /// Fractional sweep position through the window, in samples.
    phase: f32,
    /// Ring-mod oscillator phase.
    osc: f32,
    effect: Effect,
}

impl VoiceFx {
    pub fn new() -> Self {
        VoiceFx {
            ring: [0.0; RING],
            write: 0,
            phase: 0.0,
            osc: 0.0,
            effect: Effect::None,
        }
    }

    pub fn set_effect(&mut self, effect: Effect) {
        if effect != self.effect {
            self.effect = effect;
            self.phase = 0.0;
            self.osc = 0.0;
        }
    }

    /// Pitch ratio for the current effect (output pitch = input pitch × ratio).
    fn ratio(&self) -> f32 {
        match self.effect {
            Effect::Deep => 0.75,     // ~-5 semitones
            Effect::Chipmunk => 1.41, // ~+6 semitones
            _ => 1.0,
        }
    }

    /// Read the ring at a fractional delay behind the write head (linear interp).
    fn tap(&self, delay: f32) -> f32 {
        let pos = self.write as f32 - 1.0 - delay;
        let pos = pos.rem_euclid(RING as f32);
        let i0 = pos as usize % RING;
        let i1 = (i0 + 1) % RING;
        let frac = pos - pos.floor();
        self.ring[i0] * (1.0 - frac) + self.ring[i1] * frac
    }

    /// Process one buffer of mono PCM16 in place.
    pub fn process(&mut self, samples: &mut [i16]) {
        match self.effect {
            Effect::None => {}
            Effect::Robot => {
                // Ring-modulate with a 60 Hz sine: metallic monotone.
                const F: f32 = 60.0 / 48_000.0;
                for s in samples.iter_mut() {
                    let m = (core::f32::consts::TAU * self.osc).sin();
                    self.osc = (self.osc + F).fract();
                    *s = ((*s as f32) * m) as i16;
                }
            }
            _ => {
                let ratio = self.ratio();
                // Delay sweeps by (1 - ratio) per sample: for ratio < 1 the taps
                // fall behind (slower replay = lower pitch); for ratio > 1 they
                // catch up through newer audio (faster replay = higher pitch).
                let step = 1.0 - ratio;
                for s in samples.iter_mut() {
                    self.ring[self.write] = *s as f32;
                    self.write = (self.write + 1) % RING;

                    self.phase = (self.phase + step).rem_euclid(WINDOW as f32);
                    let d1 = self.phase;
                    let d2 = (self.phase + WINDOW as f32 / 2.0) % WINDOW as f32;
                    // Triangular crossfade: a tap is silent exactly when it jumps.
                    let x = d1 / WINDOW as f32;
                    let g1 = 1.0 - (2.0 * x - 1.0).abs();
                    let g2 = 1.0 - g1;

                    let out = self.tap(d1) * g1 + self.tap(d2) * g2;
                    *s = out.clamp(-32768.0, 32767.0) as i16;
                }
            }
        }
    }
}

impl Default for VoiceFx {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sine(n: usize, freq: f32) -> Vec<i16> {
        (0..n)
            .map(|i| ((core::f32::consts::TAU * freq * i as f32 / 48_000.0).sin() * 12_000.0) as i16)
            .collect()
    }

    /// Estimate frequency by counting zero crossings.
    fn zero_crossings(s: &[i16]) -> usize {
        s.windows(2).filter(|w| (w[0] < 0) != (w[1] < 0)).count()
    }

    #[test]
    fn none_is_passthrough() {
        let mut fx = VoiceFx::new();
        let orig = sine(4800, 440.0);
        let mut buf = orig.clone();
        fx.process(&mut buf);
        assert_eq!(buf, orig);
    }

    #[test]
    fn deep_lowers_pitch() {
        let mut fx = VoiceFx::new();
        fx.set_effect(Effect::Deep);
        let mut buf = sine(48_000, 440.0);
        fx.process(&mut buf);
        // Skip the first window (fill transient), then compare crossing rates.
        let shifted = zero_crossings(&buf[WINDOW * 2..]);
        let original = zero_crossings(&sine(48_000, 440.0)[WINDOW * 2..]);
        let ratio = shifted as f32 / original as f32;
        assert!((0.65..0.85).contains(&ratio), "pitch ratio {ratio} not ~0.75");
    }

    #[test]
    fn chipmunk_raises_pitch() {
        let mut fx = VoiceFx::new();
        fx.set_effect(Effect::Chipmunk);
        let mut buf = sine(48_000, 440.0);
        fx.process(&mut buf);
        let shifted = zero_crossings(&buf[WINDOW * 2..]);
        let original = zero_crossings(&sine(48_000, 440.0)[WINDOW * 2..]);
        let ratio = shifted as f32 / original as f32;
        assert!((1.25..1.6).contains(&ratio), "pitch ratio {ratio} not ~1.41");
    }

    #[test]
    fn robot_produces_output_without_clipping_silence() {
        let mut fx = VoiceFx::new();
        fx.set_effect(Effect::Robot);
        let mut buf = sine(9600, 200.0);
        fx.process(&mut buf);
        let energy: f64 = buf.iter().map(|&s| (s as f64).powi(2)).sum();
        assert!(energy > 0.0, "robot output should not be silence");
    }

    #[test]
    fn effect_switching_is_safe() {
        let mut fx = VoiceFx::new();
        let mut buf = sine(4800, 300.0);
        for e in [Effect::Deep, Effect::Robot, Effect::Chipmunk, Effect::None] {
            fx.set_effect(e);
            fx.process(&mut buf);
        }
        // No panics, no NaN garbage (values are i16 by construction).
    }
}
