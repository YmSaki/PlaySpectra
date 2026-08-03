// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Quaternion helpers for the ergonomic yaw/pitch/roll interface. The LAYER is the single source of
// truth for injected poses (sticky state); tools that need the current pose (vr_move, later
// vr_look_at) query it via pose_get/head_get rather than mirroring state, so they always act on
// what is really held.
export type Quat = { x: number; y: number; z: number; w: number };
export const IDENTITY: Quat = { x: 0, y: 0, z: 0, w: 1 };

export function normQuat(q: Quat): Quat {
  const n = Math.hypot(q.x, q.y, q.z, q.w);
  if (n < 1e-8) return { ...IDENTITY };
  return { x: q.x / n, y: q.y / n, z: q.z / n, w: q.w / n };
}
export function quatAxis(ax: number, ay: number, az: number, deg: number): Quat {
  const h = (deg * Math.PI) / 180 / 2;
  const s = Math.sin(h);
  return { x: ax * s, y: ay * s, z: az * s, w: Math.cos(h) };
}
export function quatMul(a: Quat, b: Quat): Quat {
  return {
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}
// yaw about +Y (turn), pitch about +X (look up/down), roll about +Z. Applied roll->pitch->yaw.
export function eulerToQuat(yaw = 0, pitch = 0, roll = 0): Quat {
  return quatMul(quatMul(quatAxis(0, 1, 0, yaw), quatAxis(1, 0, 0, pitch)), quatAxis(0, 0, 1, roll));
}
// Resolve a (normalized) orientation from EITHER a raw quaternion OR yaw/pitch/roll (never both),
// else identity. Returns an error string if both families are supplied (silently dropping one is a
// footgun). Output is always unit-length so downstream math (vr_look_at) stays well-defined.
export function orientationFrom(a: {
  qx?: number; qy?: number; qz?: number; qw?: number;
  yaw?: number; pitch?: number; roll?: number;
}): { ok: true; q: Quat } | { ok: false; error: string } {
  const hasQuat = a.qx !== undefined || a.qy !== undefined || a.qz !== undefined || a.qw !== undefined;
  const hasEuler = a.yaw !== undefined || a.pitch !== undefined || a.roll !== undefined;
  if (hasQuat && hasEuler)
    return { ok: false, error: "provide EITHER a quaternion (qx..qw) OR yaw/pitch/roll, not both" };
  if (hasQuat) return { ok: true, q: normQuat({ x: a.qx ?? 0, y: a.qy ?? 0, z: a.qz ?? 0, w: a.qw ?? 1 }) };
  if (hasEuler) return { ok: true, q: normQuat(eulerToQuat(a.yaw, a.pitch, a.roll)) };
  return { ok: true, q: { ...IDENTITY } };
}

// --- vector + look-rotation helpers (vr_point_at / vr_look_at) ---
export type Vec3 = { x: number; y: number; z: number };
export const vsub = (a: Vec3, b: Vec3): Vec3 => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
export const vdot = (a: Vec3, b: Vec3): number => a.x * b.x + a.y * b.y + a.z * b.z;
export const vcross = (a: Vec3, b: Vec3): Vec3 => ({
  x: a.y * b.z - a.z * b.y,
  y: a.z * b.x - a.x * b.z,
  z: a.x * b.y - a.y * b.x,
});
export function vnorm(a: Vec3): Vec3 {
  const n = Math.hypot(a.x, a.y, a.z);
  return n < 1e-8 ? { x: 0, y: 0, z: -1 } : { x: a.x / n, y: a.y / n, z: a.z / n };
}

// Orientation whose FORWARD (OpenXR -Z) axis points from `from` to `target`, +Y roughly up.
// Returns null if from==target (no direction). Builds an orthonormal basis then converts to quat.
export function lookQuat(from: Vec3, target: Vec3): Quat | null {
  const dir = vsub(target, from);
  if (Math.hypot(dir.x, dir.y, dir.z) < 1e-8) return null;
  const fwd = vnorm(dir);
  const zA = { x: -fwd.x, y: -fwd.y, z: -fwd.z }; // pose +Z is backward, so +Z = -forward
  let up: Vec3 = { x: 0, y: 1, z: 0 };
  if (Math.abs(vdot(fwd, up)) > 0.999) up = { x: 0, y: 0, z: -1 }; // looking near-vertical
  const xA = vnorm(vcross(up, zA));
  const yA = vcross(zA, xA);
  // Columns [xA yA zA] form the rotation matrix; convert to quaternion.
  const m00 = xA.x, m10 = xA.y, m20 = xA.z;
  const m01 = yA.x, m11 = yA.y, m21 = yA.z;
  const m02 = zA.x, m12 = zA.y, m22 = zA.z;
  const tr = m00 + m11 + m22;
  let q: Quat;
  if (tr > 0) {
    const s = 0.5 / Math.sqrt(tr + 1);
    q = { w: 0.25 / s, x: (m21 - m12) * s, y: (m02 - m20) * s, z: (m10 - m01) * s };
  } else if (m00 > m11 && m00 > m22) {
    const s = 2 * Math.sqrt(1 + m00 - m11 - m22);
    q = { w: (m21 - m12) / s, x: 0.25 * s, y: (m01 + m10) / s, z: (m02 + m20) / s };
  } else if (m11 > m22) {
    const s = 2 * Math.sqrt(1 + m11 - m00 - m22);
    q = { w: (m02 - m20) / s, x: (m01 + m10) / s, y: 0.25 * s, z: (m12 + m21) / s };
  } else {
    const s = 2 * Math.sqrt(1 + m22 - m00 - m11);
    q = { w: (m10 - m01) / s, x: (m02 + m20) / s, y: (m12 + m21) / s, z: 0.25 * s };
  }
  return normQuat(q);
}
