// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Unit tests for the pure quaternion/vector math in math.ts. Fully environment-independent
// (no layer, no socket, no GPU) -- these are the first tests wired into CI.
// Run: npm test  (node --import tsx --test).
import { describe, test } from "node:test";
import assert from "node:assert/strict";
import {
  IDENTITY,
  normQuat,
  quatAxis,
  quatMul,
  eulerToQuat,
  orientationFrom,
  vsub,
  vdot,
  vcross,
  vnorm,
  lookQuat,
  type Quat,
  type Vec3,
} from "./math.js";

const EPS = 1e-6;
const approx = (a: number, b: number, eps = EPS) => Math.abs(a - b) <= eps;

function assertQuat(actual: Quat, exp: Quat, eps = EPS, msg = "") {
  assert.ok(
    approx(actual.x, exp.x, eps) &&
      approx(actual.y, exp.y, eps) &&
      approx(actual.z, exp.z, eps) &&
      approx(actual.w, exp.w, eps),
    `${msg} got ${JSON.stringify(actual)} want ~${JSON.stringify(exp)}`,
  );
}
function assertVec(actual: Vec3, exp: Vec3, eps = EPS, msg = "") {
  assert.ok(
    approx(actual.x, exp.x, eps) && approx(actual.y, exp.y, eps) && approx(actual.z, exp.z, eps),
    `${msg} got ${JSON.stringify(actual)} want ~${JSON.stringify(exp)}`,
  );
}
const qlen = (q: Quat) => Math.hypot(q.x, q.y, q.z, q.w);

// Rotate a vector by a (unit) quaternion: v' = q (v,0) q*. Lets us property-test lookQuat
// by the direction it actually produces, independent of the q/-q double cover.
function rotate(q: Quat, v: Vec3): Vec3 {
  const conj: Quat = { x: -q.x, y: -q.y, z: -q.z, w: q.w };
  const r = quatMul(quatMul(q, { x: v.x, y: v.y, z: v.z, w: 0 }), conj);
  return { x: r.x, y: r.y, z: r.z };
}
const POSE_FWD: Vec3 = { x: 0, y: 0, z: -1 }; // OpenXR forward

describe("normQuat", () => {
  test("identity stays identity", () => assertQuat(normQuat(IDENTITY), IDENTITY));
  test("scales to unit length", () => assertQuat(normQuat({ x: 0, y: 0, z: 0, w: 2 }), IDENTITY));
  test("arbitrary quat becomes unit length", () => {
    assert.ok(approx(qlen(normQuat({ x: 1, y: 2, z: 3, w: 4 })), 1));
  });
  test("degenerate (zero) quat falls back to identity", () =>
    assertQuat(normQuat({ x: 0, y: 0, z: 0, w: 0 }), IDENTITY));
  test("below-threshold magnitude (n < 1e-8) falls back to identity", () =>
    assertQuat(normQuat({ x: 1e-9, y: 0, z: 0, w: 0 }), IDENTITY));
  test("fallback returns a copy, not the shared IDENTITY reference", () => {
    const r = normQuat({ x: 0, y: 0, z: 0, w: 0 });
    r.w = 999;
    assert.equal(IDENTITY.w, 1, "mutating the result must not corrupt IDENTITY");
  });
});

describe("quatAxis", () => {
  test("0 degrees is identity", () => assertQuat(quatAxis(0, 1, 0, 0), IDENTITY));
  test("180 about +Y", () => assertQuat(quatAxis(0, 1, 0, 180), { x: 0, y: 1, z: 0, w: 0 }));
  test("90 about +X", () =>
    assertQuat(quatAxis(1, 0, 0, 90), { x: Math.SQRT1_2, y: 0, z: 0, w: Math.SQRT1_2 }));
});

describe("quatMul", () => {
  const q: Quat = normQuat({ x: 0.3, y: -0.5, z: 0.2, w: 0.8 });
  test("identity is neutral on both sides", () => {
    assertQuat(quatMul(IDENTITY, q), q);
    assertQuat(quatMul(q, IDENTITY), q);
  });
  test("i*j = k (Hamilton product)", () =>
    assertQuat(quatMul({ x: 1, y: 0, z: 0, w: 0 }, { x: 0, y: 1, z: 0, w: 0 }), {
      x: 0,
      y: 0,
      z: 1,
      w: 0,
    }));
  test("non-commutative: j*i = -k", () =>
    assertQuat(quatMul({ x: 0, y: 1, z: 0, w: 0 }, { x: 1, y: 0, z: 0, w: 0 }), {
      x: 0,
      y: 0,
      z: -1,
      w: 0,
    }));
});

describe("eulerToQuat", () => {
  test("all zero (and defaults) is identity", () => {
    assertQuat(eulerToQuat(0, 0, 0), IDENTITY);
    assertQuat(eulerToQuat(), IDENTITY);
  });
  test("pure yaw equals quatAxis about +Y", () =>
    assertQuat(eulerToQuat(37), quatAxis(0, 1, 0, 37)));
  test("composition is quatMul(quatMul(yaw, pitch), roll)", () => {
    const expected = quatMul(
      quatMul(quatAxis(0, 1, 0, 20), quatAxis(1, 0, 0, 10)),
      quatAxis(0, 0, 1, 5),
    );
    assertQuat(eulerToQuat(20, 10, 5), expected);
  });
});

describe("orientationFrom", () => {
  test("neither family -> ok identity", () => {
    const r = orientationFrom({});
    assert.ok(r.ok);
    if (r.ok) assertQuat(r.q, IDENTITY);
  });
  test("quat only -> ok, normalized", () => {
    const r = orientationFrom({ qw: 2 });
    assert.ok(r.ok);
    if (r.ok) assertQuat(r.q, IDENTITY); // {0,0,0,2} normalized, missing fields default (qw=1 unused here)
  });
  test("partial quat (only qx) still counts as the quat family", () => {
    const r = orientationFrom({ qx: 1 }); // qy/qz default 0, qw defaults 1 -> {1,0,0,1} normalized
    assert.ok(r.ok);
    if (r.ok) assertQuat(r.q, normQuat({ x: 1, y: 0, z: 0, w: 1 }));
  });
  test("euler only -> ok, equals normalized eulerToQuat", () => {
    const r = orientationFrom({ yaw: 30, pitch: 15 });
    assert.ok(r.ok);
    if (r.ok) assertQuat(r.q, normQuat(eulerToQuat(30, 15, 0)));
  });
  test("both families supplied -> error (footgun prevention)", () => {
    const r = orientationFrom({ qw: 1, yaw: 10 });
    assert.equal(r.ok, false);
    if (!r.ok) assert.match(r.error, /EITHER/);
  });
  test("both families, one field each -> still error", () => {
    const r = orientationFrom({ qx: 0.1, roll: 5 });
    assert.equal(r.ok, false);
  });
});

describe("vector helpers", () => {
  test("vsub", () => assertVec(vsub({ x: 1, y: 2, z: 3 }, { x: 1, y: 1, z: 1 }), { x: 0, y: 1, z: 2 }));
  test("vdot orthogonal is 0", () =>
    assert.equal(vdot({ x: 1, y: 0, z: 0 }, { x: 0, y: 1, z: 0 }), 0));
  test("vdot general", () =>
    assert.equal(vdot({ x: 1, y: 2, z: 3 }, { x: 4, y: -5, z: 6 }), 1 * 4 + 2 * -5 + 3 * 6));
  test("vcross x*y = z", () =>
    assertVec(vcross({ x: 1, y: 0, z: 0 }, { x: 0, y: 1, z: 0 }), { x: 0, y: 0, z: 1 }));
  test("vcross is anti-commutative: y*x = -z", () =>
    assertVec(vcross({ x: 0, y: 1, z: 0 }, { x: 1, y: 0, z: 0 }), { x: 0, y: 0, z: -1 }));
});

describe("vnorm", () => {
  test("scales to unit length", () => assertVec(vnorm({ x: 0, y: 0, z: -5 }), { x: 0, y: 0, z: -1 }));
  test("degenerate (zero) falls back to OpenXR forward -Z", () =>
    assertVec(vnorm({ x: 0, y: 0, z: 0 }), { x: 0, y: 0, z: -1 }));
  test("below-threshold magnitude falls back to -Z", () =>
    assertVec(vnorm({ x: 1e-9, y: 0, z: 0 }), { x: 0, y: 0, z: -1 }));
});

describe("lookQuat", () => {
  test("from == target -> null (no direction)", () =>
    assert.equal(lookQuat({ x: 1, y: 1, z: 1 }, { x: 1, y: 1, z: 1 }), null));
  test("sub-threshold separation -> null", () =>
    assert.equal(lookQuat({ x: 0, y: 0, z: 0 }, { x: 1e-9, y: 0, z: 0 }), null));

  test("looking straight forward -Z yields ~identity", () => {
    const q = lookQuat({ x: 0, y: 0, z: 0 }, { x: 0, y: 0, z: -1 });
    assert.notEqual(q, null);
    assertQuat(q!, IDENTITY);
  });

  // Property test: for any target, rotating the pose-forward (-Z) by the returned quat must yield
  // the unit direction from `from` to `target`. Diverse directions exercise all four rotation-
  // matrix->quaternion branches (trace>0, m00, m11, m22) plus the near-vertical up-vector switch (L68).
  const from: Vec3 = { x: 0, y: 0, z: 0 };
  const targets: Vec3[] = [
    { x: 0, y: 0, z: -1 }, // forward   (trace>0)
    { x: 0, y: 0, z: 1 }, //  backward  (m11 branch)
    { x: 1, y: 0, z: 0 }, //  right
    { x: -1, y: 0, z: 0 }, // left
    { x: 0, y: 1, z: 0 }, //  straight up   -> near-vertical up-switch
    { x: 0, y: -1, z: 0 }, // straight down -> near-vertical up-switch
    { x: 1, y: 1, z: -1 }, // diagonal
    { x: -2, y: 0.5, z: 3 }, // arbitrary
    { x: 0.3, y: -0.7, z: -0.2 }, // arbitrary
  ];
  for (const t of targets) {
    test(`points -Z toward target ${JSON.stringify(t)} and stays unit-length`, () => {
      const q = lookQuat(from, t);
      assert.notEqual(q, null, "expected a rotation");
      assert.ok(approx(qlen(q!), 1), `quat must be unit length, len=${qlen(q!)}`);
      assertVec(rotate(q!, POSE_FWD), vnorm(vsub(t, from)), 1e-5, "forward should aim at target");
    });
  }

  test("works from a non-origin viewpoint", () => {
    const q = lookQuat({ x: 1, y: 2, z: 3 }, { x: 1, y: 2, z: 0 }); // dir = -Z
    assert.notEqual(q, null);
    assertVec(rotate(q!, POSE_FWD), { x: 0, y: 0, z: -1 }, 1e-5);
  });
});
