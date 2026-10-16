// ARIA - helicopter airframe adapter: the only place that touches the native
// helicopter components and the physics body.
//
// Reforger 1.8 exposes no collective/cyclic input to script, and vanilla AI
// cannot fly. Like Bohemia's own cinematic helicopter code, ARIA keeps the
// native simulation running (engine, rotor RPM, sounds, damage) and drives the
// physics body directly on the server:
//
//   flightModel 0 (VELOCITY, default): velocity is set to v + a*dt every frame
//                  (a = controller acceleration). Rock solid, frame-rate safe.
//   flightModel 1 (FORCES, experimental): rotor thrust applied as an impulse
//                  along the real rotor axis; motion comes purely from tilt.
//
// In both models the attitude is flown with angular velocity commands, so the
// airframe pitches and banks exactly like a real helicopter would.

enum ARIA_EFlightModel
{
	VELOCITY,
	FORCES
}

class ARIA_HeliAirframe
{
	protected IEntity m_Helicopter;
	protected HelicopterControllerComponent m_Controller;
	protected VehicleHelicopterSimulation m_Simulation;
	protected Physics m_Physics;
	protected bool m_bSpooledUp;

	//------------------------------------------------------------------------------------------------
	void ARIA_HeliAirframe(IEntity helicopter)
	{
		m_Helicopter = helicopter;
		m_Controller = ARIA_VehicleUtils.GetHeliController(helicopter);
		m_Simulation = ARIA_VehicleUtils.GetHeliSimulation(helicopter);
		m_Physics = helicopter.GetPhysics();
	}

	//------------------------------------------------------------------------------------------------
	bool IsValid()
	{
		return m_Helicopter && m_Controller && m_Simulation && m_Physics;
	}

	//------------------------------------------------------------------------------------------------
	IEntity GetEntity()
	{
		return m_Helicopter;
	}

	//------------------------------------------------------------------------------------------------
	void ReadState(notnull ARIA_FlightState state)
	{
		vector mat[4];
		m_Helicopter.GetWorldTransform(mat);
		state.m_vRight = mat[0];
		state.m_vUp = mat[1];
		state.m_vForward = mat[2];
		state.m_vPosition = mat[3];
		state.m_vVelocity = m_Physics.GetVelocity();
		state.m_vAngularVelocity = m_Physics.GetAngularVelocity();
		state.m_fMass = Math.Max(m_Physics.GetMass(), 1);
		state.m_fGroundY = ARIA_WorldQuery.GetSurfaceY(state.m_vPosition);
		state.m_bGroundContact = m_Simulation.HasAnyGroundContact();
	}

	//------------------------------------------------------------------------------------------------
	//! Altitude above ground/objects as measured by the native simulation.
	float GetAltitudeAGL()
	{
		return m_Simulation.GetAltitudeAGL();
	}

	//------------------------------------------------------------------------------------------------
	void Apply(ARIA_FlightOutput output, ARIA_FlightState state, float dt, int flightModel)
	{
		if (!output.m_bApply)
			return;

		if (flightModel == ARIA_EFlightModel.FORCES)
		{
			m_Physics.ApplyImpulse(output.m_vImpulse);
		}
		else
		{
			// Physics integrates gravity after we set the velocity, so pre-compensate it here.
			vector velocity = state.m_vVelocity + output.m_vDesiredAccel * dt + vector.Up * (ARIA_Math.GRAVITY * dt);
			m_Physics.SetVelocity(velocity);
		}

		m_Physics.SetAngularVelocity(output.m_vAngularVelocity);

		if (!m_Physics.IsActive())
			m_Physics.SetActive(ActiveState.ACTIVE);
	}

	//------------------------------------------------------------------------------------------------
	//! Gently damps residual motion when parked so the airframe does not creep.
	void HoldOnGround()
	{
		m_Controller.SetPersistentWheelBrake(true);
	}

	//------------------------------------------------------------------------------------------------
	void ReleaseGround()
	{
		m_Controller.SetPersistentWheelBrake(false);
	}

	//------------------------------------------------------------------------------------------------
	//! While ARIA flies, the native rotors keep spinning (visuals, sound, damage) but produce no
	//! lift or torque, so the controller is the only thing moving the airframe. Re-applied every
	//! decision because starting the engine may reset the scales to the prefab defaults.
	void SetNativeRotorSuppressed(bool suppressed)
	{
		float scale = 1;
		if (suppressed)
			scale = 0;

		int count = m_Simulation.RotorCount();
		for (int i = 0; i < count; i++)
		{
			m_Simulation.RotorSetForceScaleState(i, scale);
			m_Simulation.RotorSetTorqueScaleState(i, scale);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Keeps the engine at full throttle while ARIA is in control.
	void MaintainThrottle()
	{
		if (m_Controller.IsEngineOn())
			m_Simulation.SetThrottle(1);
	}

	//------------------------------------------------------------------------------------------------
	//! Gives the airframe back to the native simulation (crash naturally / player control).
	void ReleaseControl()
	{
		if (!IsValid())
			return;

		SetNativeRotorSuppressed(false);
		m_Controller.SetPersistentWheelBrake(false);

		// Restoring 1.0 would erase rotor damage: let the vanilla damage manager re-apply
		// the real per-rotor efficiency.
		SCR_VehicleDamageManagerComponent damage = SCR_VehicleDamageManagerComponent.Cast(m_Helicopter.FindComponent(SCR_VehicleDamageManagerComponent));
		if (damage)
			damage.UpdateVehicleState();
	}

	//------------------------------------------------------------------------------------------------
	void StartEngine()
	{
		if (!m_Controller.IsEngineOn())
		{
			if (!m_Controller.StartEngine())
				m_Simulation.EngineStart();
		}

		m_Simulation.SetThrottle(1);
		m_Controller.SetAutohoverEnabled(false);
	}

	//------------------------------------------------------------------------------------------------
	void StopEngine()
	{
		m_Simulation.SetThrottle(0);
		m_Controller.StopEngine(false);
		m_bSpooledUp = false;
	}

	//------------------------------------------------------------------------------------------------
	bool IsEngineOn()
	{
		return m_Controller.IsEngineOn();
	}

	//------------------------------------------------------------------------------------------------
	//! Main rotor RPM / target RPM (0..1+). 1 when the rotor data is not available.
	float GetRotorSpoolRatio()
	{
		if (m_Simulation.RotorCount() <= 0)
			return 1;

		float target = m_Simulation.RotorGetRPMTarget(0);
		if (target <= 1)
			return 1;

		return m_Simulation.RotorGetRPM(0) / target;
	}

	//------------------------------------------------------------------------------------------------
	void MarkSpooledUp()
	{
		m_bSpooledUp = true;
	}

	//------------------------------------------------------------------------------------------------
	//! Engine went off (ours or someone else's doing): the rotor must spool up again.
	void ClearSpooledUp()
	{
		m_bSpooledUp = false;
	}

	//------------------------------------------------------------------------------------------------
	//! Airframe can no longer fly: destroyed, or rotor wound down after having been spooled up.
	bool IsCrippled()
	{
		if (ARIA_EntityUtils.IsDeadOrDestroyed(m_Helicopter))
			return true;

		SCR_VehicleDamageManagerComponent damage = SCR_VehicleDamageManagerComponent.Cast(m_Helicopter.FindComponent(SCR_VehicleDamageManagerComponent));
		if (damage && !damage.GetEngineFunctional())
			return true;

		// Rotor wound down while the engine runs = rotor destroyed
		if (m_bSpooledUp && m_Controller.IsEngineOn() && GetRotorSpoolRatio() < 0.5)
			return true;

		return false;
	}

	//------------------------------------------------------------------------------------------------
	float GetHealth()
	{
		return ARIA_EntityUtils.GetHealthScaled(m_Helicopter);
	}
}
