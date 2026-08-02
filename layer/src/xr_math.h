// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

#ifndef PLAYSPECTRA_XR_MATH_H
#define PLAYSPECTRA_XR_MATH_H

#include <cmath>

#include <openxr/openxr.h>

namespace playspectra {

inline XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
  return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrQuaternionf QConj(const XrQuaternionf& q) { return XrQuaternionf{-q.x, -q.y, -q.z, q.w}; }
inline XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v) {
  XrQuaternionf p{v.x, v.y, v.z, 0.0f};
  XrQuaternionf r = QMul(QMul(q, p), QConj(q));  // unit-quat rotation: q * (v,0) * q*
  return XrVector3f{r.x, r.y, r.z};
}
inline XrVector3f VAdd(const XrVector3f& a, const XrVector3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f VSub(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// Normalize a quaternion in place. Values arriving over JSON carry no unit-length guarantee, and a
// non-unit quaternion both violates the XrPosef spec and makes the layer's rotation math scale by
// |q|^2 (distorting the very IPD the head-rebase preserves). Degenerate (zero) -> identity.
inline void NormalizeQuat(float& x, float& y, float& z, float& w) {
  float n = std::sqrt(x * x + y * y + z * z + w * w);
  if (n < 1e-8f) { x = y = z = 0.0f; w = 1.0f; return; }
  x /= n; y /= n; z /= n; w /= n;
}

}  // namespace playspectra

#endif  // PLAYSPECTRA_XR_MATH_H
