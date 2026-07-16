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
  // colorType 0 (greyscale) unpacks identically to palette: 1 channel, possibly sub-byte bit depth;
  // "distinct colours" = distinct grey levels, which still serves the flat-fill check.
  const isPalette = colorType === 3 || colorType === 0;
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
  const expectedApi = process.env.VR_GFX_API || "Vulkan";
  check(`capture.api == ${expectedApi}`, cap.api === expectedApi, cap.api);
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

  // (b2) durationMs glide: a head move with durationMs is IN FLIGHT partway through (neither at the
  // start nor at the target) and settles ON the target after the duration -- the pose_animator path
  // end to end (control channel field -> animator eval inside xrLocateViews -> published view).
  // Bounds are generous because view poses are per-eye (head +/- half-IPD) and sleeps are inexact.
  await rpc({ cmd: "head", x: 0, y: 1.3, z: 0, qw: 1 });         // anchor the glide start (snap)
  await sleep(500);                                               // let a few locates latch it
  await rpc({ cmd: "head", x: 2.0, y: 1.3, z: 0, qw: 1, durationMs: 5000 });
  await sleep(1200);                                              // ~24% into the 5 s glide
  const vg = await rpc({ cmd: "view" });
  const pgm = vg.views?.[0]?.pose || { x: 99 };
  check("durationMs: glide is mid-flight (between start and target)", pgm.x > 0.08 && pgm.x < 1.8,
        `x=${pgm.x?.toFixed(3)} (start 0, target 2.0)`);
  await sleep(4500);                                              // well past the 5 s mark
  const ve = await rpc({ cmd: "view" });
  const pge = ve.views?.[0]?.pose || { x: 99 };
  check("durationMs: glide settles on the target", Math.abs(pge.x - 2.0) < 0.08,
        `x=${pge.x?.toFixed(3)} (target 2.0, tolerance covers the per-eye half-IPD offset)`);
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

  // (d2) MSAA resolve (R08): when the harness forces a multisampled swapchain via
  // HELLO_XR_SAMPLE_COUNT (patched hello_xr, see setup_helloxr_msvc.sh), the capture must have gone
  // through the resolve path and report it -- a silent single-sample fallback would hide a broken
  // resolve. Skipped (not emitted) when the env is unset: stock hello_xr is always single-sample.
  const wantSamples = parseInt(process.env.HELLO_XR_SAMPLE_COUNT || "1", 10);
  if (wantSamples > 1) {
    check(`MSAA capture: sampleCount == ${wantSamples} and msaaResolved == true`,
          shot.sampleCount === wantSamples && shot.msaaResolved === true,
          JSON.stringify({ sampleCount: shot.sampleCount, msaaResolved: shot.msaaResolved }));
  }

  // (d3) HDR decode (R10): when the harness forces the 16F swapchain via HELLO_XR_HDR (patched
  // hello_xr, see setup_helloxr_msvc.sh), the capture must have gone through the half->sRGB decode
  // and say so. If the runtime does not enumerate R16G16B16A16_FLOAT, hello_xr falls back to an
  // 8-bit format -- that is a runtime capability limit, not a layer defect, so it is an explicit
  // SKIP (printed, not failed). Both current runtimes (metasim/monado) do enumerate 16F, so the
  // SKIP branch is defensive for other runtimes. Not emitted at all when the env is unset.
  const wantHdr = process.env.HELLO_XR_HDR === "1";
  if (wantHdr) {
    const DXGI_R16G16B16A16_FLOAT = 10;
    if (shot.format === DXGI_R16G16B16A16_FLOAT) {
      check("HDR capture: tonemapped == true with colorConversion recorded",
            shot.tonemapped === true && typeof shot.colorConversion === "string" &&
            shot.sourceHdrFormat === DXGI_R16G16B16A16_FLOAT,
            JSON.stringify({ tonemapped: shot.tonemapped, colorConversion: shot.colorConversion }));
    } else {
      console.log(`  SKIP HDR assertions: runtime does not enumerate R16G16B16A16_FLOAT ` +
                  `(hello_xr fell back to format ${shot.format})`);
    }
  }

  // (e) depth-request path degrades honestly. Meta sim submits no XrCompositionLayerDepthInfoKHR, so
  // this exercises the withDepth parse -> dispatch -> ResolveDepth honest {available:false} degrade
  // and must not error/crash. NOTE: the Vulkan depth-READBACK body (VulkanReadbackDepthToPng) is NOT
  // reached here (no depth swapchain) and stays review-only -- documented, not silently assumed green.
  const dshot = await rpc({ cmd: "screenshot", eye: "dominant", withDepth: true, timeoutMs: 8000 });
  check("withDepth request degrades gracefully (color ok, depth honest available:false)",
        dshot.ok === true && dshot.depth != null && typeof dshot.depth.available === "boolean",
        JSON.stringify({ ok: dshot.ok, depthAvailable: dshot.depth?.available, note: dshot.depth?.note }));

  console.log(`\n[integration] PASS=${pass} FAIL=${fail}`);
  sock.end();
  process.exit(fail > 0 ? 2 : 0);
}
main().catch((e) => { console.error("[integration] error:", e.message); process.exit(1); });
