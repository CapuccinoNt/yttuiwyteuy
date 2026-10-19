// ARIA - helicopter flight controller (pure math, no engine calls).
//
// Cascaded control, the same structure real autopilots use:
//
//   position / altitude target  --(nav)-->  desired velocity
//   desired velocity            --(PID)-->  desired acceleration
//   desired acceleration + g    ------->    required thrust vector F = m (a + g)
//   F direction (tilt-limited)  ------->    desired rotor-disc "up" axis
//   attitude + heading error    ------->    commanded angular velocity
//   |F| projected on real up axis ----->    collective thrust
//
// Horizontal motion is produced the physically correct way: by tilting the
// rotor disc. A small optional "assist" force helps precise hovering when the
// native simulation adds drag or wind the model does not know about.
//
// ARIA_HeliAirframe (Aviation/ARIA_HeliAirframe.c) reads the physics state into
// ARIA_FlightState and applies ARIA_FlightOutput back to the entity.

//------------------------------------------------------------------------------------------------
class ARIA_FlightState
{
	vector m_vPosition;
	vector m_vVelocity;
	vector m_vAngularVelocity;	//!< world space, rad/s
	vector m_vRight;
	vector m_vUp;
	vector m_vForward;
	float m_fMass = 1;
	float m_fGroundY;			//!< terrain/surface height directly below
	bool m_bGroundContact;

	//------------------------------------------------------------------------------------------------
	float GetAGL()
	{
		return m_vPosition[1] - m_fGroundY;
	}

	//------------------------------------------------------------------------------------------------
	float GetHorizontalSpeed()
	{
		return ARIA_Math.HorizontalLength(m_vVelocity);
	}

	//------------------------------------------------------------------------------------------------
	//! Heading of the nose on the horizontal plane.
	vector GetHeadingDir()
	{
		return ARIA_Math.SafeNormalize(ARIA_Math.Flatten(m_vForward), vector.Forward);
	}
}

//------------------------------------------------------------------------------------------------
enum ARIA_EHeadingMode
{
	VELOCITY,	//!< nose follows the flight path (falls back to hold when slow)
	TARGET,		//!< nose points at m_vLookAt
	FIXED,		//!< nose holds m_fHeadingYaw
	HOLD		//!< keep current heading
}

//------------------------------------------------------------------------------------------------
//! What the navigation layer asks the controller for, every frame.
class ARIA_FlightCommand
{
	bool m_bActive;					//!< false = controller outputs nothing (parked / crashed)
	bool m_bVelocityMode;			//!< true = track m_vDesiredVelocity, false = go to m_vTargetPosition
	vector m_vTargetPosition;
	vector m_vDesiredVelocity;		//!< horizontal velocity when m_bVelocityMode
	float m_fTargetAltitude;		//!< absolute world Y
	float m_fMaxSpeed = 30;			//!< horizontal speed limit (m/s)
	float m_fArrivalDecel = 2.5;	//!< braking used near the target (m/s2)
	float m_fMaxClimb = 8;
	float m_fMaxDescent = 6;
	bool m_bDirectVerticalSpeed;	//!< true = use m_fVerticalSpeed instead of altitude hold
	float m_fVerticalSpeed;

	ARIA_EHeadingMode m_eHeadingMode = ARIA_EHeadingMode.VELOCITY;
	vector m_vLookAt;
	float m_fHeadingYaw;

	//------------------------------------------------------------------------------------------------
	void Reset()
	{
		m_bActive = false;
		m_bVelocityMode = false;
		m_bDirectVerticalSpeed = false;
		m_fVerticalSpeed = 0;
		m_eHeadingMode = ARIA_EHeadingMode.VELOCITY;
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_FlightOutput
{
	bool m_bApply;
	vector m_vAngularVelocity;	//!< world space angular velocity to set (rad/s)
	vector m_vImpulse;			//!< world space impulse to apply this frame (N*s)

	// Diagnostics (debug HUD / logs)
	vector m_vDesiredVelocity;
	vector m_vDesiredAccel;
	float m_fThrust;
	float m_fTiltError;
}

//------------------------------------------------------------------------------------------------
class ARIA_FlightController
{
	protected ref ARIA_VectorPID m_VelocityPID;
	protected ARIA_HeliSettings m_Settings;
	protected float m_fHeldYaw;
	protected bool m_bHasHeldYaw;

	// Max angular rate the controller may command on the pitch/roll axes (rad/s)
	protected static const float MAX_TILT_RATE = 1.2;

	//------------------------------------------------------------------------------------------------
	void ARIA_FlightController(ARIA_HeliSettings settings)
	{
		m_Settings = settings;
		RebuildGains();
	}

	//------------------------------------------------------------------------------------------------
	//! Call after settings changed at runtime.
	void RebuildGains()
	{
		float kp = m_Settings.m_fVelocityGain;
		float ki = m_Settings.m_fVelocityIntegral;
		m_VelocityPID = new ARIA_VectorPID(kp, ki, 0, 6, m_Settings.m_fMaxAccel * 2);
		// Vertical axis needs a stronger integral (absorbs unknown lift) and up to 2 g of authority
		// so a helicopter adopted while falling can still arrest its sink.
		m_VelocityPID.SetVerticalGains(kp * 1.4, ki * 2.0, 0, 12, ARIA_Math.GRAVITY * 2);
	}

	//------------------------------------------------------------------------------------------------
	void Reset()
	{
		m_VelocityPID.Reset();
		m_bHasHeldYaw = false;
	}

	//------------------------------------------------------------------------------------------------
	void Update(ARIA_FlightState state, ARIA_FlightCommand cmd, float dt, out ARIA_FlightOutput output)
	{
		output.m_bApply = false;
		if (!cmd.m_bActive || dt <= 0)
		{
			m_VelocityPID.Reset();
			return;
		}

		// 1. Desired velocity ------------------------------------------------------------------
		vector desiredVel = ComputeDesiredHorizontalVelocity(state, cmd);
		desiredVel[1] = ComputeDesiredVerticalSpeed(state, cmd);

		// 2. Desired acceleration ----------------------------------------------------------------
		vector accel = m_VelocityPID.Update(desiredVel, state.m_vVelocity, dt);
		accel = ARIA_Math.ClampHorizontal(accel, m_Settings.m_fMaxAccel);

		// 3. Required rotor thrust vector --------------------------------------------------------
		float g = ARIA_Math.GRAVITY;
		vector required = (accel + vector.Up * g) * state.m_fMass;

		// 4. Desired disc orientation, limited by max tilt ---------------------------------------
		vector desiredUp = ARIA_Math.LimitAngleFrom(vector.Up, required, m_Settings.m_fMaxTiltDeg);

		// 5. Collective: thrust along the REAL up axis -------------------------------------------
		float thrust = ARIA_Math.Dot(required, state.m_vUp);
		float maxThrust = m_Settings.m_fMaxThrustG * g * state.m_fMass;
		thrust = Math.Clamp(thrust, 0, maxThrust);

		vector impulse = state.m_vUp * (thrust * dt);

		// Optional horizontal assist for precise station keeping
		float assist = m_Settings.m_fHorizontalAssist;
		if (assist > 0)
		{
			vector horizontalAccel = ARIA_Math.Flatten(accel);
			impulse = impulse + horizontalAccel * (state.m_fMass * assist * dt);
		}

		// 6. Attitude -> angular velocity -----------------------------------------------------
		vector tiltAxis = ARIA_Math.Cross(state.m_vUp, desiredUp); // |tiltAxis| = sin(error)
		vector omegaTilt = ARIA_Math.ClampLength(tiltAxis * m_Settings.m_fAttitudeGain, MAX_TILT_RATE);

		float yawErrorRad = ComputeYawError(state, cmd, desiredVel) * Math.DEG2RAD;
		float maxYawRate = m_Settings.m_fMaxYawRateDeg * Math.DEG2RAD;
		float yawRate = Math.Clamp(yawErrorRad * m_Settings.m_fYawGain, -maxYawRate, maxYawRate);
		vector omegaYaw = vector.Up * yawRate;

		vector omegaCmd = omegaTilt + omegaYaw;
		float k = ARIA_Math.SmoothFactor(m_Settings.m_fAngularResponse, dt);
		vector omega = state.m_vAngularVelocity + (omegaCmd - state.m_vAngularVelocity) * k;

		output.m_bApply = true;
		output.m_vAngularVelocity = omega;
		output.m_vImpulse = impulse;
		output.m_vDesiredVelocity = desiredVel;
		output.m_vDesiredAccel = accel;
		output.m_fThrust = thrust;
		output.m_fTiltError = ARIA_Math.AngleBetween(state.m_vUp, desiredUp);
	}

	//------------------------------------------------------------------------------------------------
	protected vector ComputeDesiredHorizontalVelocity(ARIA_FlightState state, ARIA_FlightCommand cmd)
	{
		if (cmd.m_bVelocityMode)
			return ARIA_Math.ClampHorizontal(ARIA_Math.Flatten(cmd.m_vDesiredVelocity), cmd.m_fMaxSpeed);

		vector toTarget = ARIA_Math.Flatten(cmd.m_vTargetPosition - state.m_vPosition);
		float dist = toTarget.Length();
		if (dist < 0.05)
			return vector.Zero;

		// Proportional near the target (precise hover), braking curve further out, cruise cap
		float speed = Math.Min(cmd.m_fMaxSpeed, ARIA_Math.ArrivalSpeed(dist, cmd.m_fArrivalDecel));
		speed = Math.Min(speed, dist * 0.8);
		return toTarget * (speed / dist);
	}

	//------------------------------------------------------------------------------------------------
	protected float ComputeDesiredVerticalSpeed(ARIA_FlightState state, ARIA_FlightCommand cmd)
	{
		if (cmd.m_bDirectVerticalSpeed)
			return Math.Clamp(cmd.m_fVerticalSpeed, -cmd.m_fMaxDescent, cmd.m_fMaxClimb);

		float error = cmd.m_fTargetAltitude - state.m_vPosition[1];
		float vz = error * m_Settings.m_fAltitudeGain;

		// Never descend fast close to the ground
		float agl = state.GetAGL();
		float maxDescent = cmd.m_fMaxDescent;
		if (agl < 40)
			maxDescent = Math.Min(maxDescent, Math.Max(1.5, agl * 0.15));

		return Math.Clamp(vz, -maxDescent, cmd.m_fMaxClimb);
	}

	//------------------------------------------------------------------------------------------------
	//! Signed heading error in degrees (positive = turn right).
	protected float ComputeYawError(ARIA_FlightState state, ARIA_FlightCommand cmd, vector desiredVel)
	{
		vector heading = state.GetHeadingDir();
		float currentYaw = ARIA_Math.YawFromDir(heading);
		float targetYaw = currentYaw;

		switch (cmd.m_eHeadingMode)
		{
			case ARIA_EHeadingMode.VELOCITY:
			{
				vector flat = ARIA_Math.Flatten(desiredVel);
				// Only turn into the flight path once we really move; hold heading in a hover
				if (flat.Length() > 4)
				{
					targetYaw = ARIA_Math.YawFromDir(flat);
					m_fHeldYaw = targetYaw;
					m_bHasHeldYaw = true;
				}
				else if (m_bHasHeldYaw)
				{
					targetYaw = m_fHeldYaw;
				}
				break;
			}

			case ARIA_EHeadingMode.TARGET:
			{
				vector toLook = ARIA_Math.Flatten(cmd.m_vLookAt - state.m_vPosition);
				if (toLook.Length() > 1)
					targetYaw = ARIA_Math.YawFromDir(toLook);
				break;
			}

			case ARIA_EHeadingMode.FIXED:
			{
				targetYaw = cmd.m_fHeadingYaw;
				break;
			}

			case ARIA_EHeadingMode.HOLD:
			{
				if (!m_bHasHeldYaw)
				{
					m_fHeldYaw = currentYaw;
					m_bHasHeldYaw = true;
				}
				targetYaw = m_fHeldYaw;
				break;
			}
		}

		// Enfusion yaw grows towards +X when looking down +Z; rotating around +Y by a positive
		// angle turns +Z towards +X, so a positive error maps to positive rotation around Up.
		return ARIA_Math.WrapAngle180(targetYaw - currentYaw);
	}
}
