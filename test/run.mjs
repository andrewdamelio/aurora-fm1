// Headless run of the browser build, as web/worklet.js drives it: audio to a WAV + level stats, screens to PNGs.
import fs from "node:fs";
import { createRequire } from "node:module";
const require = createRequire("/home/claude/tools/");
const { PNG } = require("pngjs");
const [,, wasmPath = "web/dist/aurora.wasm", secs = "30", script = ""] = process.argv;
const out = "test/out"; fs.mkdirSync(out, { recursive: true });
let ex, data, dataSize, rewinding = false, started = false, tick = 0;
const env = {
  web_yield() {
    if (rewinding) { ex.asyncify_stop_rewind(); rewinding = false; return; }
    const h = new Int32Array(ex.memory.buffer, data, 2); h[0] = data + 8; h[1] = data + dataSize;
    ex.asyncify_start_unwind(data);
  },
  web_ticks: () => (tick += 2400) >>> 0,
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { env });
ex = instance.exports; data = ex.web_async_buf(); dataSize = ex.web_async_size();
ex.web_master(700);
function run() {
  if (started) { rewinding = true; ex.asyncify_start_rewind(data); }
  started = true; ex.web_run();
  if (ex.asyncify_get_state() === 1) ex.asyncify_stop_unwind();
}
function shot(name) {
  const fb = new Uint16Array(ex.memory.buffer, ex.web_fb(), 240 * 240), png = new PNG({ width: 480, height: 480 });
  for (let y = 0; y < 480; y++) for (let x = 0; x < 480; x++) {
    const p = fb[(y >> 1) * 240 + (x >> 1)], o = (y * 480 + x) * 4;
    png.data[o] = ((p >> 11) & 31) * 255 / 31; png.data[o + 1] = ((p >> 5) & 63) * 255 / 63; png.data[o + 2] = (p & 31) * 255 / 31; png.data[o + 3] = 255;
  }
  fs.writeFileSync(`${out}/${name}.png`, PNG.sync.write(png));
}
// script: "t:cmd:arg,..."  cmds: enc:<e>:<n>  btn:<mask>  keys:<mask>  shot:<name>
const evs = script.split(",").filter(Boolean).map((s) => { const [t, c, a, b] = s.split(":"); return { t: +t, c, a, b }; }).sort((x, y) => x.t - y.t);
const N = Math.round(+secs * 44100 / 128), L = new Float32Array(N * 128), R = new Float32Array(N * 128);
const t0 = Date.now(); let maxQ = 0;
for (let q = 0; q < N; q++) {
  const t = q * 128 / 44100;
  while (evs.length && evs[0].t <= t) {
    const e = evs.shift();
    if (e.c === "enc") ex.web_enc(+e.a, +e.b); else if (e.c === "btn") ex.web_buttons(+e.a >>> 0);
    else if (e.c === "keys") ex.web_keys(+e.a >>> 0); else if (e.c === "shot") shot(e.a);
  }
  const q0 = performance.now();
  ex.web_want_frames(128);
  if (!ex.web_ready()) run();
  ex.web_take();
  maxQ = Math.max(maxQ, performance.now() - q0);
  L.set(new Float32Array(ex.memory.buffer, ex.web_out_l(), 128), q * 128);
  R.set(new Float32Array(ex.memory.buffer, ex.web_out_r(), 128), q * 128);
}
const wall = (Date.now() - t0) / 1000;
// stats per 5 s window: peak and RMS in dBFS (float full scale = 1.0; the device's ceiling is 0.5 = -6 dBFS)
const db = (v) => (20 * Math.log10(Math.max(v, 1e-9))).toFixed(1);
for (let w = 0; w * 5 < +secs; w++) {
  let pk = 0, sq = 0, dc = 0, n = 0, bad = 0;
  for (let i = w * 5 * 44100; i < Math.min(L.length, (w + 1) * 5 * 44100); i++) {
    const a = Math.max(Math.abs(L[i]), Math.abs(R[i])); pk = Math.max(pk, a); sq += L[i] * L[i] + R[i] * R[i]; dc += L[i]; n++;
    if (!Number.isFinite(L[i])) bad++;
  }
  console.log(`${String(w * 5).padStart(3)}s  peak ${db(pk)} dB  rms ${db(Math.sqrt(sq / n / 2))} dB  dc ${(dc / n).toExponential(1)} ${bad ? "NaN!" : ""}`);
}
let click = 0; for (let i = 1; i < L.length; i++) click = Math.max(click, Math.abs(L[i] - L[i - 1]));
console.log(`max sample step ${click.toFixed(4)}  | ${secs}s rendered in ${wall.toFixed(2)}s wall (${(wall / +secs * 100).toFixed(1)}% of real time), worst quantum ${maxQ.toFixed(2)} ms, device ms ${ex.web_now_ms()}, draws ${ex.web_screen_draws()}`);
const wav = Buffer.alloc(44 + L.length * 4);
wav.write("RIFF", 0); wav.writeUInt32LE(36 + L.length * 4, 4); wav.write("WAVEfmt ", 8); wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20);
wav.writeUInt16LE(2, 22); wav.writeUInt32LE(44100, 24); wav.writeUInt32LE(176400, 28); wav.writeUInt16LE(4, 32); wav.writeUInt16LE(16, 34);
wav.write("data", 36); wav.writeUInt32LE(L.length * 4, 40);
for (let i = 0; i < L.length; i++) { wav.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(L[i] * 65534))), 44 + i * 4); wav.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(R[i] * 65534))), 46 + i * 4); }
fs.writeFileSync(`${out}/aurora.wav`, wav);
shot("final");
