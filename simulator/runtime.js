// Loads chibi.wasm and provides the board: clock, NVS, events. Works in the browser and in node.
async function createDevice(wasmBytes, opts = {}) {
  const nvs = opts.nvs || new Map();
  let mem, inst, t0 = (typeof performance !== 'undefined' ? performance.now() : Date.now());
  const clockNow = opts.now || (() => (typeof performance !== 'undefined' ? performance.now() : Date.now()));
  const wallNow = opts.wall || (() => Date.now() - new Date().getTimezoneOffset() * 60000);   // local time as "UTC"
  const u8 = () => new Uint8Array(mem.buffer), dv = () => new DataView(mem.buffer);
  const cstr = p => { const b = u8(); let e = p; while (b[e]) e++; return new TextDecoder().decode(b.subarray(p, e)); };
  let out = '';
  const wasi = {
    fd_write(fd, iovs, n, nw) {
      const d = dv(); let tot = 0;
      for (let i = 0; i < n; i++) { const p = d.getUint32(iovs + i * 8, true), l = d.getUint32(iovs + i * 8 + 4, true); out += new TextDecoder().decode(u8().subarray(p, p + l)); tot += l; }
      d.setUint32(nw, tot, true);
      let k; while ((k = out.indexOf('\n')) >= 0) { (opts.log || console.log)(out.slice(0, k)); out = out.slice(k + 1); }
      return 0;
    },
    clock_time_get(id, prec, ptr) {
      const ns = id === 0 ? BigInt(Math.round(wallNow())) * 1000000n : BigInt(Math.round((clockNow() - t0) * 1e6));
      dv().setBigUint64(ptr, ns, true); return 0;
    },
    fd_close() { return 0; }, fd_seek() { return 70; }, fd_fdstat_get() { return 8; },
    proc_exit(c) { throw new Error('exit ' + c); },
  };
  const sim = {
    now_us: () => (clockNow() - t0) * 1000,
    random: () => (Math.random() * 4294967296) >>> 0,
    nvs_get(k, buf, cap) { const v = nvs.get(cstr(k)); if (!v) return -1; if (buf && cap) u8().set(v.subarray(0, Math.min(cap, v.length)), buf); return v.length; },
    nvs_set(k, buf, len) { nvs.set(cstr(k), u8().slice(buf, buf + len)); opts.onSave && opts.onSave(nvs); },
    event(kind, arg) { opts.onEvent && opts.onEvent(kind, arg); },
  };
  inst = (await WebAssembly.instantiate(wasmBytes, { wasi_snapshot_preview1: wasi, sim })).instance;
  mem = inst.exports.memory;
  if (inst.exports._initialize) inst.exports._initialize();
  const X = inst.exports;
  return {
    X, nvs, cstr,
    boot: o => X.sim_boot(o),
    frame: () => X.sim_frame(),
    // RGBA pixels of the screen as the viewer sees it
    pixels(rgba) {
      const w = X.sim_w(), h = X.sim_h(), st = X.sim_stride(), p = X.sim_fb(), b = u8();
      if (!rgba || rgba.length !== w * h * 4) rgba = new Uint8ClampedArray(w * h * 4);
      for (let y = 0; y < h; y++) { let s = p + y * st, o = y * w * 4;
        for (let x = 0; x < w; x++, s += 2, o += 4) { const v = b[s] | (b[s + 1] << 8);
          rgba[o] = ((v >> 11) & 31) * 255 / 31; rgba[o + 1] = ((v >> 5) & 63) * 255 / 63; rgba[o + 2] = (v & 31) * 255 / 31; rgba[o + 3] = 255; } }
      return { w, h, rgba };
    },
    setStr(s) { const e = new TextEncoder().encode(s.slice(0, 60)); const p = X.sim_strbuf(); u8().set(e, p); u8()[p + e.length] = 0; },
    tasks() { const n = X.sim_task_count(), a = []; for (let i = 0; i < n; i++) a.push({ id: X.sim_task_id(i), done: !!X.sim_task_done(i), name: cstr(X.sim_task_name(i)) }); return a; },
  };
}
if (typeof module !== 'undefined') module.exports = { createDevice };
