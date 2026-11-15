// SPDX-License-Identifier: GPL-3.0-only
// AURORA in an AudioWorklet. After GR!MM's web/worklet.js (Copyright (C) 2026 GR!MM), itself after Felucca's
// (hugelton/Felucca, Copyright (C) 2026 Leo Kuroshita, Hügelton Instruments) and X0X's (charlesvestal/fm1-x0x), all
// GPL-3.0. Changed for AURORA: the module comes as WebAssembly or, where that is refused, as plain JavaScript
// (aurora-js.js: globalThis.AURORA_ASM); the screen goes to the page a whole frame at a time; no SysEx.
// Each render quantum asks the firmware for its frames, which runs the device's clock forward. The app's main loop
// never returns: it pauses in web_yield (Asyncify) once the frames are there, and the next quantum resumes it.

class Aurora extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ex = null;
    this.started = false;
    this.rewinding = false;
    this.sentDraws = -1;
    this.sentLeds = "";
    this.sentWrites = 0;
    this.lastFrame = -1;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  async onMessage(m) {
    if (m.type === "load") {
      try {
        const now = globalThis.performance ? () => globalThis.performance.now() : () => Date.now();
        const env = { web_yield: () => this.yieldNow(), web_ticks: () => (now() * 24000) >>> 0 };   // fm1_ticks: 24 MHz
        let ex;
        if (m.js) {
          if (!globalThis.AURORA_ASM) throw new Error("the JavaScript build is not loaded");
          ex = globalThis.AURORA_ASM({ env });
        } else {
          ex = (await WebAssembly.instantiate(m.wasm, { env })).instance.exports;
        }
        if (ex._initialize) ex._initialize();
        this.data = ex.web_async_buf();
        this.dataSize = ex.web_async_size();
        if (m.settings) {                                    // what the page kept, before the app boots
          const b = new Uint8Array(m.settings).subarray(0, 256);
          new Uint8Array(ex.memory.buffer, ex.web_settings(), b.length).set(b);
          ex.web_settings_set_len(b.length);
        }
        ex.web_master(m.master ?? 900);
        this.ex = ex;
        this.port.postMessage({ type: "ready", js: !!m.js });
      } catch (err) {
        this.ex = null;
        this.port.postMessage({ type: "error", message: String((err && err.message) || err) });
      }
      return;
    }
    const ex = this.ex;
    if (!ex) return;
    if (m.type === "keys") ex.web_keys(m.mask >>> 0);
    else if (m.type === "buttons") ex.web_buttons(m.mask >>> 0);
    else if (m.type === "enc") ex.web_enc(m.enc, m.n | 0);
    else if (m.type === "master") ex.web_master(m.value | 0);
    else if (m.type === "midi") ex.web_midi(m.data[0], m.data[1], m.data[2]);
  }

  yieldNow() {
    const ex = this.ex;
    if (this.rewinding) {
      ex.asyncify_stop_rewind();
      this.rewinding = false;
      return;
    }
    const h = new Int32Array(ex.memory.buffer, this.data, 2);
    h[0] = this.data + 8;
    h[1] = this.data + this.dataSize;
    ex.asyncify_start_unwind(this.data);
  }

  run() {
    const ex = this.ex;
    if (this.started) {
      this.rewinding = true;
      ex.asyncify_start_rewind(this.data);
    }
    this.started = true;
    ex.web_run();
    if (ex.asyncify_get_state() === 1) ex.asyncify_stop_unwind();
  }

  publish(force) {
    const ex = this.ex, msg = { type: "frame" }, transfer = [];
    const draws = ex.web_screen_draws();
    // the firmware sends a frame as 10 strips: the page gets it once the last is in (or after a while, whatever is there)
    if (draws !== this.sentDraws && (Math.floor(draws / 10) !== Math.floor(this.sentDraws / 10) || force)) {
      this.sentDraws = draws;
      msg.fb = new Uint16Array(ex.memory.buffer, ex.web_fb(), 240 * 240).slice();
      transfer.push(msg.fb.buffer);
    }
    const leds = [ex.web_lit_buttons() >>> 0, ex.web_lit_keys() >>> 0];
    if (leds.join() !== this.sentLeds) {
      this.sentLeds = leds.join();
      msg.leds = leds;
    }
    const writes = ex.web_settings_writes();
    if (writes !== this.sentWrites) {
      this.sentWrites = writes;
      const n = ex.web_settings_len();
      if (n >= 0) msg.settings = new Uint8Array(ex.memory.buffer, ex.web_settings(), n).slice();
    }
    if (msg.fb || msg.leds || msg.settings) this.port.postMessage(msg, transfer);
    return !!msg.fb;
  }

  process(inputs, outputs) {
    const ex = this.ex;
    if (!ex) return true;
    const out = outputs[0], n = out[0].length;
    ex.web_want_frames(n);
    if (!ex.web_ready()) this.run();
    ex.web_take();
    out[0].set(new Float32Array(ex.memory.buffer, ex.web_out_l(), n));
    if (out[1]) out[1].set(new Float32Array(ex.memory.buffer, ex.web_out_r(), n));
    if (currentTime - this.lastFrame >= 1 / 40) {
      if (this.publish(currentTime - this.lastFrame > 0.25)) this.lastFrame = currentTime;
    }
    return true;
  }
}

registerProcessor("aurora", Aurora);
