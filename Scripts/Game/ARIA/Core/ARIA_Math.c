// ARIA - Advanced Reforger Intelligent AI
// Pure math helpers: PID controllers, angle and vector utilities.
// Nothing in this file touches engine APIs beyond Math/vector, so it is the
// safest place to look when tuning controller behaviour.
//
// Enfusion convention reminder: Y is UP. X = right, Z = forward.

//------------------------------------------------------------------------------------------------
//! Scalar PID controller with integral clamp and derivative-on-measurement.
class ARIA_PID
{
	float m_fKp;
	float m_fKi;
	float m_fKd;
	float m_fIntegralLimit;
	float m_fOutputLimit;

	protected float m_fIntegral;
	protected float m_fLastMeasurement;
	protected bool m_bHasLast;

	//------------------------------------------------------------------------------------------------
	void ARIA_PID(float kp, float ki, float kd, float integralLimit, float outputLimit)
	{
		m_fKp = kp;
		m_fKi = ki;
		m_fKd = kd;
		m_fIntegralLimit = integralLimit;
		m_fOutputLimit = outputLimit;
	}

	//------------------------------------------------------------------------------------------------
	void Reset()
	{
		m_fIntegral = 0;
		m_fLastMeasurement = 0;
		m_bHasLast = false;
	}

	//------------------------------------------------------------------------------------------------
	float GetIntegral()
	{
		return m_fIntegral;
	}

	//------------------------------------------------------------------------------------------------
	//! \param setpoint desired value
	//! \param measurement current value
	//! \param dt seconds since last update
	float Update(float setpoint, float measurement, float dt)
	{
		if (dt <= 0)
			return 0;

		float error = setpoint - measurement;

		m_fIntegral += error * dt;
		m_fIntegral = Math.Clamp(m_fIntegral, -m_fIntegralLimit, m_fIntegralLimit);

		float derivative = 0;
		if (m_bHasLast)
			derivative = -(measurement - m_fLastMeasurement) / dt;

		m_fLastMeasurement = measurement;
		m_bHasLast = true;

		float output = m_fKp * error + m_fKi * m_fIntegral + m_fKd * derivative;
		return Math.Clamp(output, -m_fOutputLimit, m_fOutputLimit);
	}
}

//------------------------------------------------------------------------------------------------
//! 3-axis PID (one ARIA_PID per world axis). Used for velocity tracking.
class ARIA_VectorPID
{
	protected ref ARIA_PID m_X;
	protected ref ARIA_PID m_Y;
	protected ref ARIA_PID m_Z;

	//------------------------------------------------------------------------------------------------
	void ARIA_VectorPID(float kp, float ki, float kd, float integralLimit, float outputLimit)
	{
		m_X = new ARIA_PID(kp, ki, kd, integralLimit, outputLimit);
		m_Y = new ARIA_PID(kp, ki, kd, integralLimit, outputLimit);
		m_Z = new ARIA_PID(kp, ki, kd, integralLimit, outputLimit);
	}

	//------------------------------------------------------------------------------------------------
	//! Allows a different gain set for the vertical (Y) axis.
	void SetVerticalGains(float kp, float ki, float kd, float integralLimit, float outputLimit)
	{
		m_Y = new ARIA_PID(kp, ki, kd, integralLimit, outputLimit);
	}

	//------------------------------------------------------------------------------------------------
	void Reset()
	{
		m_X.Reset();
		m_Y.Reset();
		m_Z.Reset();
	}

	//------------------------------------------------------------------------------------------------
	vector Update(vector setpoint, vector measurement, float dt)
	{
		vector result;
		result[0] = m_X.Update(setpoint[0], measurement[0], dt);
		result[1] = m_Y.Update(setpoint[1], measurement[1], dt);
		result[2] = m_Z.Update(setpoint[2], measurement[2], dt);
		return result;
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_Math
{
	static const float GRAVITY = 9.81;

	//------------------------------------------------------------------------------------------------
	//! Wraps an angle in degrees to [-180, 180).
	static float WrapAngle180(float deg)
	{
		while (deg >= 180)
			deg -= 360;

		while (deg < -180)
			deg += 360;

		return deg;
	}

	//------------------------------------------------------------------------------------------------
	//! Horizontal (XZ) component of a vector.
	static vector Flatten(vector v)
	{
		return Vector(v[0], 0, v[2]);
	}

	//------------------------------------------------------------------------------------------------
	static float HorizontalLength(vector v)
	{
		return Math.Sqrt(v[0] * v[0] + v[2] * v[2]);
	}

	//------------------------------------------------------------------------------------------------
	static float HorizontalDistance(vector a, vector b)
	{
		float dx = a[0] - b[0];
		float dz = a[2] - b[2];
		return Math.Sqrt(dx * dx + dz * dz);
	}

	//------------------------------------------------------------------------------------------------
	//! Returns v scaled down so its length does not exceed maxLength.
	static vector ClampLength(vector v, float maxLength)
	{
		float len = v.Length();
		if (len <= maxLength || len < 0.0001)
			return v;

		return v * (maxLength / len);
	}

	//------------------------------------------------------------------------------------------------
	//! Same as ClampLength but only on the horizontal plane, Y untouched.
	static vector ClampHorizontal(vector v, float maxLength)
	{
		float len = HorizontalLength(v);
		if (len <= maxLength || len < 0.0001)
			return v;

		float k = maxLength / len;
		return Vector(v[0] * k, v[1], v[2] * k);
	}

	//------------------------------------------------------------------------------------------------
	static vector SafeNormalize(vector v, vector fallback)
	{
		float len = v.Length();
		if (len < 0.0001)
			return fallback;

		return v * (1 / len);
	}

	//------------------------------------------------------------------------------------------------
	//! Cross product a x b.
	static vector Cross(vector a, vector b)
	{
		return Vector(
			a[1] * b[2] - a[2] * b[1],
			a[2] * b[0] - a[0] * b[2],
			a[0] * b[1] - a[1] * b[0]);
	}

	//------------------------------------------------------------------------------------------------
	static float Dot(vector a, vector b)
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	//------------------------------------------------------------------------------------------------
	//! Angle in degrees between two vectors.
	static float AngleBetween(vector a, vector b)
	{
		float la = a.Length();
		float lb = b.Length();
		if (la < 0.0001 || lb < 0.0001)
			return 0;

		float c = Math.Clamp(Dot(a, b) / (la * lb), -1, 1);
		return Math.Acos(c) * Math.RAD2DEG;
	}

	//------------------------------------------------------------------------------------------------
	//! Rotates unit vector `from` towards `to` by at most maxAngleDeg (on the plane they span).
	//! Used to limit the helicopter's commanded tilt.
	static vector LimitAngleFrom(vector reference, vector dir, float maxAngleDeg)
	{
		vector r = SafeNormalize(reference, vector.Up);
		vector d = SafeNormalize(dir, r);
		float angle = AngleBetween(r, d);
		if (angle <= maxAngleDeg)
			return d;

		// Orthogonal component of d relative to r
		vector ortho = d - r * Dot(d, r);
		if (ortho.Length() < 0.0001)
			return r;

		ortho = ortho * (1 / ortho.Length());
		float rad = maxAngleDeg * Math.DEG2RAD;
		return r * Math.Cos(rad) + ortho * Math.Sin(rad);
	}

	//------------------------------------------------------------------------------------------------
	//! Unit direction on the XZ plane for a given yaw in degrees (0 = +Z, 90 = +X).
	static vector DirFromYaw(float yawDeg)
	{
		float rad = yawDeg * Math.DEG2RAD;
		return Vector(Math.Sin(rad), 0, Math.Cos(rad));
	}

	//------------------------------------------------------------------------------------------------
	//! Yaw in degrees of a direction on the XZ plane (0 = +Z, 90 = +X).
	static float YawFromDir(vector dir)
	{
		return Math.Atan2(dir[0], dir[2]) * Math.RAD2DEG;
	}

	//------------------------------------------------------------------------------------------------
	//! Perpendicular (to the right) of a horizontal direction.
	static vector RightOf(vector dir)
	{
		return Vector(dir[2], 0, -dir[0]);
	}

	//------------------------------------------------------------------------------------------------
	//! Exponential smoothing factor that is frame-rate independent.
	//! \param rate how fast to converge (1/s)
	static float SmoothFactor(float rate, float dt)
	{
		return Math.Clamp(1 - Math.Pow(2.718281828, -rate * dt), 0, 1);
	}

	//------------------------------------------------------------------------------------------------
	//! Max speed at which you can still stop within `distance` with deceleration `decel`.
	static float ArrivalSpeed(float distance, float decel)
	{
		if (distance <= 0)
			return 0;

		return Math.Sqrt(2 * decel * distance);
	}

	//------------------------------------------------------------------------------------------------
	//! Random point inside a ring around center (horizontal only).
	static vector RandomPointInRing(vector center, float minRadius, float maxRadius)
	{
		float yaw = Math.RandomFloat(0, 360);
		float r = Math.RandomFloat(minRadius, maxRadius);
		vector p = center + DirFromYaw(yaw) * r;
		return p;
	}

	//------------------------------------------------------------------------------------------------
	//! Returns true with the given probability [0..1].
	static bool Chance(float probability)
	{
		return Math.RandomFloat(0, 1) < probability;
	}
}
