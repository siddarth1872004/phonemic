// AudioWorklet (runs on the browser's audio thread). Collects the mic into
// fixed 10 ms frames, converts to PCM16 and hands each frame to the page.
class PcmWorklet extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.frame = (options.processorOptions && options.processorOptions.frame) || 480;
    this.buf = new Int16Array(this.frame);
    this.n = 0;
    this.peak = 0;
    this.muted = false;
    this.port.onmessage = (e) => { if (e.data && 'muted' in e.data) this.muted = !!e.data.muted; };
  }

  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (!ch) return true;
    for (let i = 0; i < ch.length; i++) {
      let s = this.muted ? 0 : ch[i];
      if (s > 1) s = 1; else if (s < -1) s = -1;
      const a = s < 0 ? -s : s;
      if (a > this.peak) this.peak = a;
      this.buf[this.n++] = Math.round(s * 32767);
      if (this.n === this.frame) {
        const pcm = this.buf.slice(0);
        this.port.postMessage({ pcm, peak: this.peak }, [pcm.buffer]);
        this.n = 0;
        this.peak = 0;
      }
    }
    return true;
  }
}
registerProcessor('pcm-worklet', PcmWorklet);
