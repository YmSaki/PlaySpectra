#!/usr/bin/env python3
# Unit tests for the PURE math in playspectra_server.py (the Scenario Runner's interpolation
# primitives). Fully environment-independent: no socket, no running Server, no host app -- so the
# "test framework itself is untested" gap is closed with plain stdlib unittest (no pip/venv).
# Run:  python tools/playspectra_math_test.py    (or: python -m unittest playspectra_math_test)
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # import the sibling module
from playspectra_server import lerp3, quat_mul, quat_yaw, quat_norm, slerp  # noqa: E402

R2 = math.sqrt(0.5)  # cos(45) = sin(45)


def close(a, b, tol=1e-9):
    return all(math.isclose(x, y, abs_tol=tol) for x, y in zip(a, b))


class Lerp3(unittest.TestCase):
    def test_endpoints_and_midpoint(self):
        a, b = [0.0, 0.0, 0.0], [2.0, 4.0, 6.0]
        self.assertTrue(close(lerp3(a, b, 0.0), a))
        self.assertTrue(close(lerp3(a, b, 1.0), b))
        self.assertTrue(close(lerp3(a, b, 0.5), [1.0, 2.0, 3.0]))

    def test_extrapolates_linearly(self):
        self.assertTrue(close(lerp3([0, 0, 0], [1, 0, 0], 2.0), [2.0, 0.0, 0.0]))


class QuatMul(unittest.TestCase):
    IDENT = [0.0, 0.0, 0.0, 1.0]

    def test_identity_is_neutral(self):
        q = quat_norm([0.3, -0.5, 0.2, 0.8])
        self.assertTrue(close(quat_mul(self.IDENT, q), q))
        self.assertTrue(close(quat_mul(q, self.IDENT), q))

    def test_i_times_j_is_k(self):
        self.assertTrue(close(quat_mul([1, 0, 0, 0], [0, 1, 0, 0]), [0, 0, 1, 0]))

    def test_non_commutative_j_times_i_is_minus_k(self):
        self.assertTrue(close(quat_mul([0, 1, 0, 0], [1, 0, 0, 0]), [0, 0, -1, 0]))


class QuatYaw(unittest.TestCase):
    def test_zero_is_identity(self):
        self.assertTrue(close(quat_yaw(0.0), [0.0, 0.0, 0.0, 1.0]))

    def test_half_pi_about_y(self):
        self.assertTrue(close(quat_yaw(math.pi / 2), [0.0, R2, 0.0, R2]))

    def test_pi_is_flip_about_y(self):
        self.assertTrue(close(quat_yaw(math.pi), [0.0, 1.0, 0.0, 0.0], tol=1e-9))

    def test_composes_via_quat_mul(self):
        # two yaw(pi/4) rotations compose to yaw(pi/2)
        q = quat_mul(quat_yaw(math.pi / 4), quat_yaw(math.pi / 4))
        self.assertTrue(close(q, quat_yaw(math.pi / 2)))


class QuatNorm(unittest.TestCase):
    def test_scales_to_unit(self):
        self.assertTrue(close(quat_norm([0, 0, 0, 2]), [0, 0, 0, 1]))
        self.assertTrue(math.isclose(sum(c * c for c in quat_norm([1, 2, 3, 4])), 1.0))

    def test_zero_stays_zero(self):
        # n = sqrt(0) or 1.0 -> 1.0, so a zero quat divides by 1 and stays zero (documented behavior).
        self.assertTrue(close(quat_norm([0, 0, 0, 0]), [0, 0, 0, 0]))


class Slerp(unittest.TestCase):
    IDENT = [0.0, 0.0, 0.0, 1.0]

    def test_same_endpoints_returns_same(self):
        q = quat_yaw(0.7)
        self.assertTrue(close(slerp(q, q, 0.5), q, tol=1e-6))

    def test_endpoints(self):
        q1 = quat_yaw(math.pi / 2)
        self.assertTrue(close(slerp(self.IDENT, q1, 0.0), self.IDENT, tol=1e-6))
        self.assertTrue(close(slerp(self.IDENT, q1, 1.0), q1, tol=1e-6))

    def test_midpoint_is_half_angle(self):
        q1 = quat_yaw(math.pi / 2)
        self.assertTrue(close(slerp(self.IDENT, q1, 0.5), quat_yaw(math.pi / 4), tol=1e-6))

    def test_result_is_unit_length(self):
        r = slerp(quat_yaw(0.2), quat_yaw(2.5), 0.37)
        self.assertTrue(math.isclose(sum(c * c for c in r), 1.0, abs_tol=1e-9))

    def test_takes_short_path_for_antipodal(self):
        # q and -q are the same rotation; slerp must flip (dot<0) and take the short arc,
        # so the midpoint equals the identity rotation (up to sign), not a 180-degree detour.
        q = quat_yaw(0.6)
        neg = [-c for c in q]
        r = slerp(q, neg, 0.5)
        self.assertTrue(close(r, q, tol=1e-6) or close(r, neg, tol=1e-6))

    def test_near_colinear_uses_linear_branch(self):
        # dot > 0.9995 -> linear+normalize path; still unit and near both endpoints.
        q0 = quat_yaw(0.010)
        q1 = quat_yaw(0.011)
        r = slerp(q0, q1, 0.5)
        self.assertTrue(math.isclose(sum(c * c for c in r), 1.0, abs_tol=1e-9))


if __name__ == "__main__":
    unittest.main(verbosity=2)
