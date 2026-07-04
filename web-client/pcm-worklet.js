// AudioWorklet: runs on the browser's dedicated audio thread (not main JS).
// Accumulates mic Float32 samples into fixed 10 ms frames, converts to PCM16,
// and posts each frame to the main thread, which frames it and sends over the
// WebSocket. This is the one place JS touches audio samples — see the principle
// #1 caveat in docs/WEB-CLIENT.md.
class PcmWorklet extends AudioWorkletProcessor {
  constructor() {
    super();
    this.FRAME = 480; // 10 ms @ 48 kHz
    this.buf = new Int16Array(this.FRAME);
    this.n = 0;
  }

  process(inputs) {
    const input = inputs[0];
    if (!input || input.length === 0) return true;
    const ch = input[0]; // mono (first channel)
    if (!ch) return true;

    let peak = 0;
    for (let i = 0; i < ch.length; i++) {
      let s = ch[i];
      if (s > 1) s = 1; else if (s < -1) s = -1;
      const a = s < 0 ? -s : s;
      if (a > peak) peak = a;
      this.buf[this.n++] = (s * 32767) | 0;

      if (this.n === this.FRAME) {
        // Copy out (the transferred buffer is detached) and hand up.
        const pcm = this.buf.slice(0, this.FRAME);
        this.port.postMessage({ pcm, peak }, [pcm.buffer]);
        this.n = 0;
        peak = 0;
      }
    }
    return true;
  }
}

registerProcessor('pcm-worklet', PcmWorklet);
