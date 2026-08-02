// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Integration assertions for the OpenVR path: hellovr_dx12 (OpenVR app) -> OpenComposite
// (openvr_api.dll drop-in, translates to OpenXR) -> playspectra layer -> Monado.
//
// Asserts the engine-independent surfaces measured in M3 (.claude/openvr-milestone-plan.md M3):
//   (a) session/frame loop through the layer: api=D3D12 (OpenComposite's client choice for this
//       DX12-submitting app), framesObserved > 0 -- NOT process liveness (journal L55)
//   (b) OpenComposite's action translation is visible: "opencomposite-actions" set, legacy-* names
//   (c) head injection moves the located views (base pose + injection compose in STAGE space)
//   (d) capture path: screenshot decodes, dims match the swapchain, pixels vary
//   (e) input-injection reachability into the app is UNPROVEN for OpenComposite's manifest routing:
//       tried honestly, reported as SKIP (with reason) when the app shows no reaction. A SKIP is
//       printed and counted -- never a silent pass (M4 contract).
//
// Usage: node integration_openvr.mjs [port]   (default 52700). Exit 0 iff no assertion FAILs.
import net from "node:net";
import fs from "node:fs";
import zlib from "node:zlib";

const PORT = Number(process.argv[2] ?? process.env.PLAYSPECTRA_PORT ?? "52700");
const HOST = "127.0.0.1";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let pass = 0, fail = 0, skip = 0;
const check = (name, cond, detail) => {
  (cond ? pass++ : fail++);
  console.log(`  ${cond ? "PASS" : "FAIL"} ${name}${detail !== undefined ? ": " + detail : ""}`);
  return cond;
};
const skipCheck = (name, reason) => {
  skip++;
  console.log(`  SKIP ${name}: ${reason}`);
};

// ---- control-channel client (NDJSON request/response, one in flight at a time) ----
function connectWithRetry(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve, reject) => {
    const attempt = () => {
      const sock = new net.Socket();
      sock.setNoDelay(true);
      sock.once("error", () => {
        if (Date.now() > deadline) reject(new Error("connect timeout"));
        else setTimeout(attempt, 300);
      });
      sock.connect(PORT, HOST, () => resolve(sock));
    };
    attempt();
  });
}
function makeRpc(sock) {
  const pending = [];
  let buf = "";
  sock.on("data", (chunk) => {
    buf += chunk.toString("utf8");
    let nl;
    while ((nl = buf.indexOf("\n")) !== -1) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (line && pending.length) pending.shift()(line);
    }
  });
  return (obj) =>
    new Promise((resolve) => {
      pending.push((l) => { try { resolve(JSON.parse(l)); } catch { resolve({ _raw: l }); } });
      sock.write(JSON.stringify(obj) + "\n");
    });
}

// ---- minimal PNG decoder (same scope as integration_hello_xr.mjs: 8-bit truecolor + palette +
// greyscale, enough for the non-degeneracy check) ----
function decodePng(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error("not a PNG");
  let off = 8, width = 0, height = 0, bitDepth = 0, colorType = 0, plteEntries = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString("ascii", off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === "IHDR") {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9];
    } else if (type === "PLTE") plteEntries = Math.floor(len / 3);
    else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
    off += 12 + len;
  }
  const isPalette = colorType === 3 || colorType === 0;   // greyscale unpacks like palette (1 ch)
  const isTrue = colorType === 2 || colorType === 6;
  if (!isPalette && !isTrue) throw new Error(`unsupported colorType ${colorType}`);
  if (isTrue && bitDepth !== 8) throw new Error(`unsupported truecolor bitDepth ${bitDepth}`);
  const channels = colorType === 6 ? 4 : colorType === 2 ? 3 : 1;
  const bpp = isPalette ? 1 : channels;
  const stride = isPalette ? Math.ceil((width * bitDepth) / 8) : width * channels;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const out = Buffer.alloc(height * stride);
  const paeth = (a, b, c) => {
    const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  };
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const rowIn = y * (stride + 1) + 1, rowOut = y * stride, prevOut = (y - 1) * stride;
    for (let x = 0; x < stride; x++) {
      const rawByte = raw[rowIn + x];
      const a = x >= bpp ? out[rowOut + x - bpp] : 0;
      const b = y > 0 ? out[prevOut + x] : 0;
      const c = x >= bpp && y > 0 ? out[prevOut + x - bpp] : 0;
      let v;
      switch (filter) {
        case 0: v = rawByte; break;
        case 1: v = rawByte + a; break;
        case 2: v = rawByte + b; break;
        case 3: v = rawByte + ((a + b) >> 1); break;
        case 4: v = rawByte + paeth(a, b, c); break;
        default: throw new Error("bad PNG filter " + filter);
      }
      out[rowOut + x] = v & 0xff;
    }
  }
  const counts = new Map();
  let sampled = 0;
  const step = Math.max(1, Math.floor(Math.min(width, height) / 64));
  const perByte = isPalette ? 8 / bitDepth : 0;
  const mask = isPalette ? (1 << bitDepth) - 1 : 0;
  for (let y = 0; y < height; y += step)
    for (let x = 0; x < width; x += step) {
      let key;
      if (isPalette) {
        const byte = out[y * stride + Math.floor(x / perByte)];
        const shift = (perByte - 1 - (x % perByte)) * bitDepth;
        key = (byte >> shift) & mask;
      } else {
        const i = y * stride + x * channels;
        key = (out[i] << 16) | (out[i + 1] << 8) | out[i + 2];
      }
      counts.set(key, (counts.get(key) || 0) + 1);
      sampled++;
    }
  let maxCount = 0;
  for (const c of counts.values()) if (c > maxCount) maxCount = c;
  return { width, height, distinctColors: counts.size, dominantFraction: maxCount / sampled };
}

async function main() {
  const sock = await connectWithRetry(25000);
  console.log("[openvr-integration] connected to", `${HOST}:${PORT}`);
  const rpc = makeRpc(sock);

  // (a) session + frame loop THROUGH OpenComposite. framesObserved>0 is the load-bearing check.
  const st = await rpc({ cmd: "status" });
  const cap = st.capture || {};
  check("status: instance+session up", st.instance === true && st.session === true,
        JSON.stringify({ instance: st.instance, session: st.session, ca: st.conformanceAutomation }));
  check("OpenComposite chose the D3D12 OpenXR client", cap.api === "D3D12", cap.api);
  check("frame loop reached (framesObserved > 0)", (cap.framesObserved || 0) > 0,
        `frames=${cap.framesObserved} proj=${cap.lastFrameHadProjection}`);
  const scs = cap.swapchains || [];
  check("two per-eye swapchains with sane dims", scs.length >= 2 && scs[0].width > 0 && scs[0].height > 0 && scs[1].width > 0 && scs[1].height > 0,
        JSON.stringify(scs.map((s) => `${s.width}x${s.height} fmt=${s.format}`)) + ` eye0=${scs[0]?.width}x${scs[0]?.height} eye1=${scs[1]?.width}x${scs[1]?.height}`);

  // (b) OpenComposite's legacy-input translation is what the layer must see (M3 recording).
  const act = await rpc({ cmd: "actions" });
  const sets = act.actionSets || [];
  const ocSet = sets.find((s) => s.name === "opencomposite-actions");
  check("action set 'opencomposite-actions' attached", !!ocSet && ocSet.attached === true,
        `sets=${sets.map((s) => s.name).join(",")}`);
  const trig = ocSet?.actions?.find((a) => a.name === "legacy-right-trigger");
  check("legacy-right-trigger exists (float)", !!trig && trig.typeName === "FLOAT_INPUT",
        trig ? trig.typeName : "absent");
  const hasSimple = (trig?.boundPaths || []).some((b) => b.profile === "/interaction_profiles/khr/simple_controller");
  check("legacy-right-trigger bound on khr/simple_controller", hasSimple,
        JSON.stringify((trig?.boundPaths || []).map((b) => b.profile)));

  // (c) head injection moves the located views (compose with the simulated HMD base pose).
  const v0 = await rpc({ cmd: "view" });
  check("view available with 2 views", v0.available === true && (v0.viewCount || 0) >= 2,
        `available=${v0.available} count=${v0.viewCount}`);
  const f = v0.views?.[0]?.fov || {};
  check("plausible projection FOV", f.angleLeft < 0 && f.angleRight > 0,
        `L=${(f.angleLeft || 0).toFixed(2)} R=${(f.angleRight || 0).toFixed(2)}`);
  const p0 = v0.views?.[0]?.pose || { x: 0, y: 0, z: 0 };
  await rpc({ cmd: "head", x: 0.6, y: 1.3, z: -0.4, qw: 1 });
  await sleep(700);
  const v1 = await rpc({ cmd: "view" });
  const p1 = v1.views?.[0]?.pose || { x: 0, y: 0, z: 0 };
  const delta = Math.hypot(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
  check("head override moves the located view pose", delta > 0.5,
        `|delta|=${delta.toFixed(3)} (${p0.x?.toFixed(2)},${p0.y?.toFixed(2)},${p0.z?.toFixed(2)} -> ${p1.x?.toFixed(2)},${p1.y?.toFixed(2)},${p1.z?.toFixed(2)})`);

  // (e) input-injection reachability trial (non-CA [G] path synthesises OpenXR action state; whether
  // OpenComposite's OpenVR-side manifest routing consumes it is what we probe). SKIP, not FAIL,
  // when the app shows no haptic reaction -- recorded as an M4 result either way.
  await rpc({ cmd: "active", hand: "right", active: true });
  const h0 = (await rpc({ cmd: "status" })).hapticCount || 0;
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    await rpc({ cmd: "input", hand: "right", input: "trigger/value", type: "float", value: 1.0 });
    await rpc({ cmd: "input", hand: "right", input: "select/click", type: "bool", value: true });
    await sleep(150);
  }
  const h1 = (await rpc({ cmd: "status" })).hapticCount || 0;
  if (h1 > h0) {
    check("injected trigger reaches the app (haptic reaction)", true, `haptics ${h0} -> ${h1}`);
  } else {
    skipCheck("injected trigger reaches the app (haptic reaction)",
              `no haptic after 3s injection (haptics ${h0} -> ${h1}); OpenComposite's manifest-action ` +
              `routing to the app is not exercised by legacy-action synthesis -- recorded for M5/real-game phase`);
  }

  // (d) capture through the OpenComposite-created D3D12 swapchain.
  const shot = await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 8000 });
  check("screenshot ok", shot.ok === true, `${shot.width}x${shot.height} fmt=${shot.format} api=${shot.api}`);
  if (shot.ok) {
    check("screenshot dims match the swapchain", shot.width === scs[0].width && shot.height === scs[0].height,
          `shot ${shot.width}x${shot.height} vs swapchain ${scs[0].width}x${scs[0].height}`);
    const png = decodePng(fs.readFileSync(shot.path));
    check("PNG decodes to the reported dimensions", png.width === shot.width && png.height === shot.height,
          `${png.width}x${png.height}`);
    check("capture is non-degenerate (not a flat fill)", png.distinctColors >= 3 && png.dominantFraction < 0.999,
          `distinctColors=${png.distinctColors} dominant=${(png.dominantFraction * 100).toFixed(1)}%`);
  }
  const dshot = await rpc({ cmd: "screenshot", eye: "dominant", withDepth: true, timeoutMs: 8000 });
  check("withDepth degrades honestly on the D3D12 path", dshot.ok === true && typeof dshot.depth?.available === "boolean",
        JSON.stringify({ ok: dshot.ok, depthAvailable: dshot.depth?.available }));

  console.log(`\n[openvr-integration] PASS=${pass} FAIL=${fail} SKIP=${skip}`);
  sock.end();
  process.exit(fail > 0 ? 2 : 0);
}
main().catch((e) => { console.error("[openvr-integration] error:", e.message); process.exit(1); });
