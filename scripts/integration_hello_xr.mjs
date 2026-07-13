// Deep integration test for the vr_agent OpenXR layer, driven against hello_xr (a real, standard
// OpenXR app) through the layer's TCP NDJSON control channel.
//
// GAP-10 asks us to validate the layer against a real engine on ENGINE-INDEPENDENT surfaces. The
// project's own Godot app does not (yet) initialise OpenXR, so per the user's decision we assert those
// same surfaces against hello_xr, which exercises the identical OpenXR paths any engine would:
//   (a) profile / binding interception  -> `actions` dumps attached action sets with bound paths
//   (b) pose/view override reaches the runtime answer -> `head` injection moves the `view` result
//   (c) sync semantics round-trip       -> injecting grab>0.9 makes hello_xr buzz -> `haptics` grows
//   (d) non-degenerate capture          -> the screenshot PNG decodes, dims match, pixels vary
//
// Usage: node integration_hello_xr.mjs [port]   (default 52700). Exit 0 iff every assertion passes.
import net from "node:net";
import fs from "node:fs";
import zlib from "node:zlib";

const PORT = Number(process.argv[2] ?? process.env.VR_AGENT_PORT ?? "52700");
const HOST = "127.0.0.1";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let pass = 0, fail = 0;
const check = (name, cond, detail) => {
  (cond ? pass++ : fail++);
  console.log(`  ${cond ? "PASS" : "FAIL"} ${name}${detail !== undefined ? ": " + detail : ""}`);
  return cond;
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

// ---- minimal PNG decoder for the non-degeneracy check. Handles what lodepng emits: 8-bit RGB/RGBA
// (truecolor) AND indexed-palette (colorType 3, 1/2/4/8-bit) -- lodepng auto-reduces a low-colour
// scene to a palette PNG, so we must decode that form too. For palette images "distinct colours" =
// distinct palette indices actually used, which is exactly the flat-fill check we want. ----
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
  const isPalette = colorType === 3;
  const isTrue = colorType === 2 || colorType === 6;
  if (!isPalette && !isTrue) throw new Error(`unsupported colorType ${colorType}`);
  if (isTrue && bitDepth !== 8) throw new Error(`unsupported truecolor bitDepth ${bitDepth}`);
  const channels = colorType === 6 ? 4 : colorType === 2 ? 3 : 1;
  const bpp = isPalette ? 1 : channels;                         // bytes per pixel used by the filters
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
  // Sample a grid of pixels; count distinct colours (truecolor) / indices (palette) and the dominant fraction.
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
        key = (byte >> shift) & mask;                          // palette index
      } else {
        const i = y * stride + x * channels;
        key = (out[i] << 16) | (out[i + 1] << 8) | out[i + 2]; // packed RGB
      }
      counts.set(key, (counts.get(key) || 0) + 1);
      sampled++;
    }
  let maxCount = 0;
  for (const c of counts.values()) if (c > maxCount) maxCount = c;
  return { width, height, palette: isPalette, plteEntries,
           distinctColors: counts.size, dominantFraction: maxCount / sampled };
}

async function main() {
  const sock = await connectWithRetry(25000);
  const rpc = makeRpc(sock);
  console.log(`[integration] connected to ${HOST}:${PORT}`);

  // 0) baseline: the layer sees a live Vulkan session submitting projection frames.
  const st = await rpc({ cmd: "status" });
  const cap = st.capture || {};
  check("status.instance", st.instance === true, JSON.stringify({ instance: st.instance, ca: st.conformanceAutomation }));
  check("capture.api == Vulkan", cap.api === "Vulkan", cap.api);
  check("projection frames flowing", (cap.framesObserved || 0) > 0 && cap.lastFrameHadProjection === true,
        `frames=${cap.framesObserved} proj=${cap.lastFrameHadProjection} views=${cap.lastFrameViewCount}`);
  const sc = (cap.swapchains && cap.swapchains[0]) || {};

  // (a) profile / binding interception: xrSuggestInteractionProfileBindings + xrAttachSessionActionSets.
  const act = await rpc({ cmd: "actions" });
  const sets = act.actionSets || [];
  const attached = sets.filter((s) => s.attached);
  const allActions = sets.flatMap((s) => s.actions || []);
  const withBindings = allActions.filter((a) => (a.boundPaths || []).length > 0);
  check("actions: >=1 attached action set", attached.length >= 1, `sets=${sets.length} attached=${attached.length}`);
  check("actions: actions carry bound interaction-profile paths", withBindings.length >= 1,
        `${withBindings.length}/${allActions.length} actions bound; e.g. ` +
        JSON.stringify((withBindings[0]?.boundPaths || []).slice(0, 2)));

  // (b) xrLocateViews override reaches the runtime answer: injected head moves the reported view pose.
  const v0 = await rpc({ cmd: "view" });
  check("view available (xrLocateViews observed)", v0.available === true && (v0.viewCount || 0) >= 1,
        `available=${v0.available} viewCount=${v0.viewCount}`);
  const fov0 = v0.views?.[0]?.fov;
  check("view exposes a plausible projection FOV", !!fov0 && fov0.angleRight > 0 && fov0.angleLeft < 0,
        fov0 ? `L=${fov0.angleLeft.toFixed(2)} R=${fov0.angleRight.toFixed(2)}` : "none");
  const p0 = v0.views?.[0]?.pose || { x: 0, y: 0, z: 0 };
  await rpc({ cmd: "head", x: 0.6, y: 1.3, z: -0.4, qw: 1 });
  await sleep(700); // let a few xrLocateViews go by
  const v1 = await rpc({ cmd: "view" });
  const p1 = v1.views?.[0]?.pose || p0;
  const moved = Math.hypot(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
  check("head override moves the located view pose", moved > 0.3,
        `|delta|=${moved.toFixed(3)} (before ${p0.x.toFixed(2)},${p0.y.toFixed(2)},${p0.z.toFixed(2)} -> after ${p1.x.toFixed(2)},${p1.y.toFixed(2)},${p1.z.toFixed(2)})`);
  await rpc({ cmd: "head_clear" });

  // (c) sync-semantics round-trip: inject grab>0.9; hello_xr responds by buzzing the controller.
  const h0 = (await rpc({ cmd: "haptics", limit: 200 })).haptics?.length || 0;
  await rpc({ cmd: "active", hand: "right", active: true });
  for (let i = 0; i < 6; i++) { await rpc({ cmd: "input", hand: "right", input: "squeeze/value", type: "float", value: 1.0 }); await sleep(180); }
  const h1 = (await rpc({ cmd: "haptics", limit: 200 })).haptics?.length || 0;
  check("sync round-trip: injected grab makes the app buzz", h1 > h0, `haptic events ${h0} -> ${h1}`);

  // controller pose override held authoritatively by the layer.
  await rpc({ cmd: "pose", hand: "right", x: 0.2, y: -0.2, z: -0.5, qw: 1 });
  const pg = await rpc({ cmd: "pose_get", hand: "right" });
  check("controller pose override held", pg.active === true && Math.abs(pg.x - 0.2) < 1e-3,
        JSON.stringify({ active: pg.active, x: pg.x, y: pg.y, z: pg.z }));

  // (d) non-degenerate capture: decode the PNG, dims match the swapchain, pixels are not a flat fill.
  const shot = await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 8000 });
  if (!check("screenshot ok", shot.ok === true, shot.ok ? `${shot.width}x${shot.height} fmt=${shot.format}` : shot.error)) {
    console.log(`\n[integration] PASS=${pass} FAIL=${fail}`); sock.end(); process.exit(2);
  }
  check("screenshot dims match the swapchain", sc.height ? shot.height === sc.height : shot.height > 100,
        `shot ${shot.width}x${shot.height} vs swapchain h=${sc.height}`);
  let png;
  try { png = decodePng(fs.readFileSync(shot.path)); }
  catch (e) { check("PNG decodes", false, e.message); }
  if (png) {
    check("PNG decodes to the reported dimensions", png.width === shot.width && png.height === shot.height,
          `${png.width}x${png.height}`);
    check("capture is non-degenerate (not a flat fill)", png.distinctColors >= 3 && png.dominantFraction < 0.999,
          `distinctColors=${png.distinctColors} dominant=${(png.dominantFraction * 100).toFixed(1)}%`);
  }

  console.log(`\n[integration] PASS=${pass} FAIL=${fail}`);
  sock.end();
  process.exit(fail > 0 ? 2 : 0);
}
main().catch((e) => { console.error("[integration] error:", e.message); process.exit(1); });
