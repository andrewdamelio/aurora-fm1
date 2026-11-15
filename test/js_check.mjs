import fs from "node:fs";
new Function(fs.readFileSync("web/dist/aurora-js.js", "utf8"))();
let ex, data, size, rew = false, started = false, tick = 0;
ex = globalThis.AURORA_ASM({ env: { web_ticks: () => (tick += 2400) >>> 0, web_yield() { if (rew) { ex.asyncify_stop_rewind(); rew = false; return; } const h = new Int32Array(ex.memory.buffer, data, 2); h[0] = data + 8; h[1] = data + size; ex.asyncify_start_unwind(data); } } });
data = ex.web_async_buf(); size = ex.web_async_size(); ex.web_master(700);
const w = (await WebAssembly.instantiate(fs.readFileSync("web/dist/aurora.wasm"), { env: { web_ticks: () => 0, web_yield() {} } })).instance.exports; void w;
let pk = 0, sum = 0; const t0 = Date.now();
for (let q = 0; q < 3445; q++) {
  ex.web_want_frames(128);
  if (!ex.web_ready()) { if (started) { rew = true; ex.asyncify_start_rewind(data); } started = true; ex.web_run(); if (ex.asyncify_get_state() === 1) ex.asyncify_stop_unwind(); }
  ex.web_take();
  const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), 128);
  for (const v of l) { pk = Math.max(pk, Math.abs(v)); sum += v * v; }
}
const fb = new Uint16Array(ex.memory.buffer, ex.web_fb(), 57600); let lit = 0; for (const p of fb) lit += p !== 0;
console.log("js fallback: 10 s in", (Date.now() - t0) / 1000, "s; peak", pk.toFixed(3), "rms", Math.sqrt(sum / 3445 / 128).toFixed(4), "lit pixels", lit);
