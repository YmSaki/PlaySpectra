package playspectra

import "math"

func Lerp3(a, b []float64, t float64) []float64 {
	if len(a) < 3 || len(b) < 3 {
		return nil
	}
	return []float64{a[0] + (b[0]-a[0])*t, a[1] + (b[1]-a[1])*t, a[2] + (b[2]-a[2])*t}
}

func QuatMul(a, b []float64) []float64 {
	if len(a) < 4 || len(b) < 4 {
		return nil
	}
	ax, ay, az, aw := a[0], a[1], a[2], a[3]
	bx, by, bz, bw := b[0], b[1], b[2], b[3]
	return []float64{
		aw*bx + ax*bw + ay*bz - az*by,
		aw*by - ax*bz + ay*bw + az*bx,
		aw*bz + ax*by - ay*bx + az*bw,
		aw*bw - ax*bx - ay*by - az*bz,
	}
}

func QuatYaw(yawRadians float64) []float64 {
	h := yawRadians * 0.5
	return []float64{0, math.Sin(h), 0, math.Cos(h)}
}

func QuatNorm(q []float64) []float64 {
	if len(q) < 4 {
		return nil
	}
	n := math.Sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3])
	if n == 0 || math.IsNaN(n) {
		n = 1
	}
	return []float64{q[0] / n, q[1] / n, q[2] / n, q[3] / n}
}

func Slerp(q0, q1 []float64, t float64) []float64 {
	a, b := QuatNorm(q0), QuatNorm(q1)
	if a == nil || b == nil {
		return nil
	}
	dot := a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]
	if dot < 0 {
		for i := range b {
			b[i] = -b[i]
		}
		dot = -dot
	}
	dot = math.Max(-1, math.Min(1, dot))
	if dot > 0.9995 {
		return QuatNorm([]float64{
			a[0] + (b[0]-a[0])*t,
			a[1] + (b[1]-a[1])*t,
			a[2] + (b[2]-a[2])*t,
			a[3] + (b[3]-a[3])*t,
		})
	}
	theta0 := math.Acos(dot)
	theta := theta0 * t
	sinTheta0 := math.Sin(theta0)
	s0 := math.Sin(theta0-theta) / sinTheta0
	s1 := math.Sin(theta) / sinTheta0
	return []float64{
		a[0]*s0 + b[0]*s1,
		a[1]*s0 + b[1]*s1,
		a[2]*s0 + b[2]*s1,
		a[3]*s0 + b[3]*s1,
	}
}

func cloneFloats(v []float64) []float64 {
	return append([]float64(nil), v...)
}
