// ARIA - AI helicopter pilot. NEW FEATURE: vanilla Reforger AI cannot fly.
//
// One ARIA_HeliPilot exists per helicopter whose pilot seat holds an AI of an
// AI-led group. It:
//
//   * takes over the crew group's waypoints and flies them (Move, Patrol,
//     Cycle, Defend = orbit, Search&Destroy/Attack = CAS orbit, Wait = hover,
//     Get Out = land and unload passengers),
//   * runs support missions given by ARIA_AirOps (airlift, close air support),
//   * starts the engine, waits for rotor spool-up, takes off, follows the
//     terrain (trees and buildings included), approaches, finds a safe LZ
//     and lands; unloads/boards squads on the ground,
//   * evades when hit, returns to base when badly damaged, hands the controls
//     to the co-pilot when the pilot dies, and lets the airframe crash
//     naturally when it can no longer fly.
//
// Door gunners are regular vanilla AI in turret seats and engage on their own;
// the orbit pattern keeps targets on the gunners' side.

enum ARIA_EHeliState
{
	PARKED,		//!< on the ground, engine off
	STARTING,	//!< engine starting, rotor spooling up
	TAKEOFF,	//!< vertical climb out
	FLYING,		//!< executing airborne tasks
	LANDING,	//!< final descent onto the LZ
	LANDED,		//!< on the ground, engine running
	DISABLED	//!< crippled / no pilot: ARIA lets go
}

class ARIA_HeliPilot
{
	protected ARIA_Manager m_Manager;
	protected IEntity m_Helicopter;
	protected ARIA_GroupBrain m_Crew;
	protected IEntity m_Pilot;

	protected ref ARIA_HeliAirframe m_Airframe;
	protected ref ARIA_FlightController m_Controller;
	protected ref ARIA_FlightState m_FlightState = new ARIA_FlightState();
	protected ref ARIA_FlightCommand m_Command = new ARIA_FlightCommand();
	protected ref ARIA_FlightOutput m_Output = new ARIA_FlightOutput();
	protected ref array<ref ARIA_HeliTask> m_aTasks = {};

	protected ARIA_EHeliState m_eState = ARIA_EHeliState.PARKED;
	protected float m_fStateTime;
	protected float m_fNextDecision;
	protected bool m_bReleased;

	// Home / takeoff
	protected vector m_vHome;
	protected bool m_bHasHome;
	protected vector m_vTakeoffPos;

	// Landing zone
	protected ref ARIA_LZSearch m_LZSearch;
	protected vector m_vLZ;
	protected bool m_bLZReady;
	protected float m_fTouchdownTime = -1;

	// Terrain following cache
	protected float m_fTerrainCeiling;
	protected float m_fNextTerrainSample;

	// Threat reaction
	protected float m_fLastHealth = 1;
	protected float m_fEvadeUntil = -1;
	protected vector m_vEvadeDir;
	protected bool m_bDamagedRTB;

	protected float m_fOrbitDirection = 1;
	protected bool m_bIdleLoiter;
	protected bool m_bStandDown;

	// Landing retries
	protected int m_iLandingFailures;
	protected ref array<vector> m_aLZBlacklist = {};

	protected static const float DECISION_INTERVAL = 0.25;
	protected static const float TAKEOFF_ALTITUDE = 15;
	protected static const float APPROACH_ALTITUDE = 30;
	protected static const float LANDED_IDLE_SHUTDOWN = 45;
	protected static const float LOITER_RADIUS = 300;
	protected static const int MAX_LANDING_ATTEMPTS = 3;

	//------------------------------------------------------------------------------------------------
	void ARIA_HeliPilot(ARIA_Manager manager, IEntity helicopter, ARIA_GroupBrain crew)
	{
		m_Manager = manager;
		m_Helicopter = helicopter;
		m_Crew = crew;
		m_Pilot = ARIA_VehicleUtils.GetPilot(helicopter);
		m_Airframe = new ARIA_HeliAirframe(helicopter);
		m_Controller = new ARIA_FlightController(manager.GetSettings().m_Heli);
		m_fLastHealth = m_Airframe.GetHealth();
		if (Math.RandomFloat(0, 1) < 0.5)
			m_fOrbitDirection = -1;

		// Never take over a wreck or a helicopter a player is flying
		if (!m_Airframe.IsValid() || m_Airframe.IsCrippled() || (m_Pilot && IsPlayer(m_Pilot)))
		{
			m_bReleased = true;
			return;
		}

		float now = manager.GetTime();
		m_Airframe.SetNativeRotorSuppressed(true);
		bool onGround = m_Airframe.GetAltitudeAGL() < 3;
		if (onGround)
		{
			m_vHome = helicopter.GetOrigin();
			m_bHasHome = true;
			if (m_Airframe.IsEngineOn())
				SetState(ARIA_EHeliState.LANDED, now);
			else
				SetState(ARIA_EHeliState.PARKED, now);
		}
		else
		{
			// Spawned in the air (Game Master, scripted insertion...): keep flying.
			// Spool-up is marked only once the rotor really reaches its RPM (see Decide).
			m_Airframe.StartEngine();
			SetState(ARIA_EHeliState.FLYING, now);
		}

		// Crew waypoints are absorbed on the first Decide(), after the pilot check passed
	}

	//==============================================================================================
	// Public API
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	bool IsValid()
	{
		return !m_bReleased && m_Helicopter && m_Airframe && m_Airframe.IsValid() && m_eState != ARIA_EHeliState.DISABLED;
	}

	//------------------------------------------------------------------------------------------------
	IEntity GetHelicopter()
	{
		return m_Helicopter;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_GroupBrain GetCrew()
	{
		return m_Crew;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_EHeliState GetState()
	{
		return m_eState;
	}

	//------------------------------------------------------------------------------------------------
	string GetDebugName()
	{
		return ARIA_VehicleUtils.GetDebugName(m_Helicopter);
	}

	//------------------------------------------------------------------------------------------------
	Faction GetFaction()
	{
		if (m_Crew)
			return m_Crew.GetFaction();

		return null;
	}

	//------------------------------------------------------------------------------------------------
	//! Free for support missions: no orders of its own, not damaged.
	bool IsAvailableForTasking()
	{
		if (!IsValid() || m_bDamagedRTB)
			return false;

		if (m_eState == ARIA_EHeliState.TAKEOFF || m_eState == ARIA_EHeliState.LANDING)
			return false;

		// Loitering after its last order, or only heading home: free to take a mission
		if (m_bIdleLoiter)
			return true;

		foreach (ARIA_HeliTask task : m_aTasks)
		{
			if (task.m_eType != ARIA_EHeliTaskType.RTB)
				return false;
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	bool IsArmed()
	{
		return ARIA_VehicleUtils.HasTurret(m_Helicopter);
	}

	//------------------------------------------------------------------------------------------------
	int GetFreeCargoSeats()
	{
		array<BaseCompartmentSlot> cargo = {};
		ARIA_VehicleUtils.GetCompartmentsOfType(m_Helicopter, ECompartmentType.CARGO, cargo);
		int count = 0;
		foreach (BaseCompartmentSlot slot : cargo)
		{
			if (!slot.GetOccupant() && slot.IsCompartmentAccessible())
				count++;
		}

		return count;
	}

	//------------------------------------------------------------------------------------------------
	//! Pick up a squad and fly it to its destination (ARIA_AirOps).
	void AssignAirlift(ARIA_GroupBrain squad, vector pickup, vector dropZone)
	{
		ClearTasks();

		ARIA_HeliTask land = ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, pickup);
		land.m_bFromAirOps = true;
		m_aTasks.Insert(land);

		ARIA_HeliTask board = ARIA_HeliTask.Create(ARIA_EHeliTaskType.BOARD, pickup);
		board.m_Squad = squad;
		board.m_bFromAirOps = true;
		board.m_fDuration = m_Manager.GetSettings().m_Heli.m_fBoardTimeout;
		m_aTasks.Insert(board);

		ARIA_HeliTask drop = ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, dropZone);
		drop.m_bFromAirOps = true;
		m_aTasks.Insert(drop);

		ARIA_HeliTask unload = ARIA_HeliTask.Create(ARIA_EHeliTaskType.UNLOAD, dropZone);
		unload.m_Squad = squad;
		unload.m_bFromAirOps = true;
		unload.m_fDuration = m_Manager.GetSettings().m_Heli.m_fUnloadTimeout;
		m_aTasks.Insert(unload);

		QueueReturnToBase();
		m_bIdleLoiter = false;
		ARIA_Log.Info(GetDebugName() + " assigned to airlift " + squad.GetDebugName());
	}

	//------------------------------------------------------------------------------------------------
	//! Orbit a target area so the door gunners can engage (ARIA_AirOps).
	void AssignCAS(vector target, float stationTime)
	{
		ClearTasks();
		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;

		ARIA_HeliTask orbit = ARIA_HeliTask.Create(ARIA_EHeliTaskType.ORBIT, target);
		orbit.m_fRadius = cfg.m_fCASOrbitRadius;
		orbit.m_fAltitude = cfg.m_fCASOrbitAltitude;
		orbit.m_fDuration = stationTime;
		orbit.m_bFromAirOps = true;
		m_aTasks.Insert(orbit);

		QueueReturnToBase();
		m_bIdleLoiter = false;
		ARIA_Log.Info(GetDebugName() + " flying close air support at " + target.ToString());
	}

	//------------------------------------------------------------------------------------------------
	//! Drops every task and flies home (or lands where it is when it has no home).
	void OrderReturnToBase()
	{
		CancelAirOpsSquads();
		ClearTasks();
		m_bIdleLoiter = false;
		if (m_bHasHome)
			QueueReturnToBase();
		else
			m_aTasks.Insert(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, m_Helicopter.GetOrigin()));
	}

	//------------------------------------------------------------------------------------------------
	//! Script API: append a task (see ARIA_Api).
	void AddTask(ARIA_HeliTask task)
	{
		if (m_bIdleLoiter)
			ClearTasks();

		m_bIdleLoiter = false;
		m_aTasks.Insert(task);
	}

	//------------------------------------------------------------------------------------------------
	void OnSettingsChanged()
	{
		m_Controller = new ARIA_FlightController(m_Manager.GetSettings().m_Heli);
	}

	//------------------------------------------------------------------------------------------------
	void Release()
	{
		if (m_bReleased)
			return;

		m_bReleased = true;
		CancelAirOpsSquads();
		if (m_Airframe)
			m_Airframe.ReleaseControl();
	}

	//------------------------------------------------------------------------------------------------
	//! ARIA helicopters were disabled at runtime: land where possible, then hand control back.
	void RequestStandDown()
	{
		if (m_bStandDown || !IsValid())
			return;

		m_bStandDown = true;
		CancelAirOpsSquads();
		ClearTasks();
		m_bIdleLoiter = false;

		bool airborne = m_eState == ARIA_EHeliState.TAKEOFF || m_eState == ARIA_EHeliState.FLYING || m_eState == ARIA_EHeliState.LANDING;
		if (airborne)
			m_aTasks.Insert(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, m_Helicopter.GetOrigin()));

		ARIA_Log.Info(GetDebugName() + " standing down");
	}

	//==============================================================================================
	// Main update
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	void Update(float now, float dt)
	{
		if (now >= m_fNextDecision)
		{
			m_fNextDecision = now + DECISION_INTERVAL;
			Decide(now);
			if (!IsValid())
				return;
		}

		Fly(now, dt);
	}

	//------------------------------------------------------------------------------------------------
	protected void Decide(float now)
	{
		if (!CheckPilotAndAirframe(now))
			return;

		if (!m_bStandDown)
			AbsorbCrewWaypoints();

		CheckThreat(now);

		// Vanilla AI or a damaged starter may have stopped the engine: restart it while flying
		bool airborne = m_eState == ARIA_EHeliState.TAKEOFF || m_eState == ARIA_EHeliState.FLYING || m_eState == ARIA_EHeliState.LANDING;
		if (airborne && !m_Airframe.IsEngineOn())
			m_Airframe.StartEngine();

		m_Airframe.SetNativeRotorSuppressed(true);
		m_Airframe.MaintainThrottle();
		if (m_Airframe.IsEngineOn() && m_Airframe.GetRotorSpoolRatio() >= 0.9)
			m_Airframe.MarkSpooledUp();

		switch (m_eState)
		{
			case ARIA_EHeliState.PARKED:
			{
				if (m_bStandDown)
				{
					Disable(now);
					return;
				}

				if (!m_aTasks.IsEmpty())
				{
					m_Airframe.StartEngine();
					SetState(ARIA_EHeliState.STARTING, now);
				}
				break;
			}

			case ARIA_EHeliState.STARTING:
			{
				// Take off once the rotor is at speed (spool-up is marked above), or after the
				// timeout on airframes whose RPM data is unusual
				float spoolUp = m_Manager.GetSettings().m_Heli.m_fSpoolUpTime;
				if (m_Airframe.GetRotorSpoolRatio() >= 0.9 || now - m_fStateTime >= spoolUp)
					BeginTakeoff(now);
				break;
			}

			case ARIA_EHeliState.TAKEOFF:
			{
				if (m_Airframe.GetAltitudeAGL() >= TAKEOFF_ALTITUDE || now - m_fStateTime > 20)
				{
					m_Controller.Reset();
					SetState(ARIA_EHeliState.FLYING, now);
				}
				break;
			}

			case ARIA_EHeliState.FLYING:
			{
				DecideFlying(now);
				break;
			}

			case ARIA_EHeliState.LANDING:
			{
				DecideLanding(now);
				break;
			}

			case ARIA_EHeliState.LANDED:
			{
				DecideLanded(now);
				break;
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void DecideFlying(float now)
	{
		ARIA_HeliTask task = GetCurrentTask(now);
		if (!task)
		{
			if (m_bStandDown)
				m_aTasks.Insert(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, m_Helicopter.GetOrigin()));
			else
				StartIdleLoiter();

			return;
		}

		switch (task.m_eType)
		{
			case ARIA_EHeliTaskType.MOVE:
			{
				float radius = Math.Max(task.m_fRadius, 60);
				float distance = ARIA_Math.HorizontalDistance(m_Helicopter.GetOrigin(), task.m_vPosition);
				if (distance < radius && (!task.m_bStopAtTarget || m_FlightState.GetHorizontalSpeed() < 6))
					CompleteTask(now);
				break;
			}

			case ARIA_EHeliTaskType.LAND:
			case ARIA_EHeliTaskType.RTB:
			{
				UpdateLZSearch(task);
				if (!m_bLZReady)
					break;

				float lzDistance = ARIA_Math.HorizontalDistance(m_Helicopter.GetOrigin(), m_vLZ);
				if (lzDistance < 20 && m_FlightState.GetHorizontalSpeed() < 5)
				{
					m_Controller.Reset();
					m_fTouchdownTime = -1;
					SetState(ARIA_EHeliState.LANDING, now);
				}
				break;
			}

			case ARIA_EHeliTaskType.ORBIT:
			case ARIA_EHeliTaskType.HOVER:
			{
				if (task.m_fDuration >= 0 && now - task.m_fStartTime >= task.m_fDuration)
					CompleteTask(now);
				break;
			}

			case ARIA_EHeliTaskType.BOARD:
			case ARIA_EHeliTaskType.UNLOAD:
			{
				// Ground tasks need a landing first
				ARIA_HeliTask land = ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, task.m_vPosition);
				land.m_bFromAirOps = task.m_bFromAirOps;
				m_aTasks.InsertAt(land, 0);
				ResetLZ();
				break;
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void DecideLanding(float now)
	{
		float agl = m_Airframe.GetAltitudeAGL();
		bool lowEnough = agl < 1.5;
		// Some airframes measure AGL from a high origin: also accept "low and not sinking any more"
		bool settled = agl < 3.5 && Math.AbsFloat(m_FlightState.m_vVelocity[1]) < 0.3 && now - m_fStateTime > 3;
		if (m_FlightState.m_bGroundContact || lowEnough || settled)
		{
			if (m_fTouchdownTime < 0)
				m_fTouchdownTime = now;

			// Settle for a moment before declaring the landing complete
			if (now - m_fTouchdownTime >= 0.75)
			{
				m_Airframe.HoldOnGround();
				ResetLandingRetries();
				SetState(ARIA_EHeliState.LANDED, now);

				ARIA_HeliTask task = GetCurrentTask(now);
				if (task && (task.m_eType == ARIA_EHeliTaskType.LAND))
					CompleteTask(now);
			}
		}
		else
		{
			m_fTouchdownTime = -1;
		}

		// Abort if something went wrong (trees, roof, water, pushed away...): blacklist the spot,
		// climb back to the approach altitude and search a wider area; give up after a few tries.
		if (now - m_fStateTime > 60)
		{
			m_iLandingFailures++;
			m_aLZBlacklist.Insert(m_vLZ);
			ResetLZ();
			m_Controller.Reset();
			SetState(ARIA_EHeliState.FLYING, now);

			if (m_iLandingFailures >= MAX_LANDING_ATTEMPTS)
				AbandonLanding(now);
			else
				ARIA_Log.Debug(string.Format("%1 landing aborted (%2/%3), searching a new LZ", GetDebugName(), m_iLandingFailures, MAX_LANDING_ATTEMPTS));
		}
	}

	//------------------------------------------------------------------------------------------------
	//! No usable LZ: drop the landing and the ground tasks that depended on it.
	protected void AbandonLanding(float now)
	{
		ARIA_Log.Warning(GetDebugName() + " could not find a place to land, skipping the landing");
		bool removedLanding;
		while (!m_aTasks.IsEmpty())
		{
			ARIA_HeliTask task = m_aTasks[0];
			bool landing = task.m_eType == ARIA_EHeliTaskType.LAND || task.m_eType == ARIA_EHeliTaskType.RTB;
			bool groundTask = task.m_eType == ARIA_EHeliTaskType.BOARD || task.m_eType == ARIA_EHeliTaskType.UNLOAD;
			if (landing && removedLanding)
				break;

			if (!landing && !groundTask)
				break;

			if (task.m_Squad && task.m_Squad.IsValid() && task.m_eType == ARIA_EHeliTaskType.UNLOAD)
				task.m_Squad.OnAirliftFinished(now);

			removedLanding = removedLanding || landing;
			m_aTasks.RemoveOrdered(0);
		}

		ResetLandingRetries();
	}

	//------------------------------------------------------------------------------------------------
	protected void ResetLandingRetries()
	{
		m_iLandingFailures = 0;
		m_aLZBlacklist.Clear();
	}

	//------------------------------------------------------------------------------------------------
	protected void DecideLanded(float now)
	{
		ARIA_HeliTask task = GetCurrentTask(now);
		if (!task && m_bStandDown)
		{
			m_Airframe.StopEngine();
			Disable(now);
			return;
		}

		if (!task)
		{
			// Nothing to do: shut down after a while to save the engine (and the server)
			if (now - m_fStateTime > LANDED_IDLE_SHUTDOWN)
			{
				m_Airframe.StopEngine();
				SetState(ARIA_EHeliState.PARKED, now);
			}
			return;
		}

		switch (task.m_eType)
		{
			case ARIA_EHeliTaskType.BOARD:
			{
				ARIA_GroupBrain squad = task.m_Squad;
				if (task.m_bFromAirOps && (!squad || !squad.IsValid()))
				{
					ARIA_Log.Debug(GetDebugName() + " passengers gone, mission cancelled");
					AbortAirOpsMission(now);
					return;
				}

				if (!task.m_bStarted)
				{
					task.m_bStarted = true;
					if (squad)
						squad.OrderBoard(m_Helicopter);
				}

				// Without an assigned squad (Get In waypoint) we simply wait for whoever boards
				bool timedOut = task.m_fDuration >= 0 && now - task.m_fStartTime >= task.m_fDuration;
				bool boarded = squad && squad.IsBoarded(m_Helicopter);
				if (boarded || timedOut)
				{
					if (timedOut && squad)
						ARIA_Log.Debug(GetDebugName() + " boarding timed out, leaving with who is aboard");

					CompleteTask(now);
				}
				return;
			}

			case ARIA_EHeliTaskType.UNLOAD:
			{
				if (!task.m_bStarted)
				{
					task.m_bStarted = true;
					UnloadPassengers(task.m_Squad);
				}

				bool unloadTimedOut = task.m_fDuration >= 0 && now - task.m_fStartTime >= task.m_fDuration;
				if (ArePassengersOut(task.m_Squad) || unloadTimedOut)
				{
					if (task.m_Squad && task.m_Squad.IsValid())
						task.m_Squad.OnAirliftFinished(now);

					CompleteTask(now);
				}
				return;
			}

			case ARIA_EHeliTaskType.RTB:
			{
				if (!IsNear(task.m_vPosition))
					break; // home is elsewhere: take off

				// Landed at home: mission complete
				CompleteTask(now);
				m_bDamagedRTB = false;
				m_Airframe.StopEngine();
				SetState(ARIA_EHeliState.PARKED, now);
				return;
			}

			case ARIA_EHeliTaskType.LAND:
			{
				if (!IsNear(task.m_vPosition))
					break; // landing ordered somewhere else: take off

				CompleteTask(now);
				return;
			}
		}

		// Next task is airborne
		BeginTakeoff(now);
	}

	//------------------------------------------------------------------------------------------------
	//! True when the helicopter already stands within LZ search distance of position.
	protected bool IsNear(vector position)
	{
		float radius = Math.Max(m_Manager.GetSettings().m_Heli.m_fLZSearchRadius, 50);
		return ARIA_Math.HorizontalDistance(m_Helicopter.GetOrigin(), position) <= radius;
	}

	//------------------------------------------------------------------------------------------------
	protected void BeginTakeoff(float now)
	{
		m_Airframe.ReleaseGround();
		m_vTakeoffPos = m_Helicopter.GetOrigin();
		m_Controller.Reset();
		ResetLZ();
		SetState(ARIA_EHeliState.TAKEOFF, now);
	}

	//==============================================================================================
	// Flight
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected void Fly(float now, float dt)
	{
		bool airborneState = m_eState == ARIA_EHeliState.TAKEOFF || m_eState == ARIA_EHeliState.FLYING || m_eState == ARIA_EHeliState.LANDING;
		if (!airborneState)
			return;

		m_Airframe.ReadState(m_FlightState);
		BuildCommand(now);
		m_Controller.Update(m_FlightState, m_Command, dt, m_Output);
		m_Airframe.Apply(m_Output, m_FlightState, dt, m_Manager.GetSettings().m_Heli.m_iFlightModel);
	}

	//------------------------------------------------------------------------------------------------
	protected void BuildCommand(float now)
	{
		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;
		m_Command.Reset();
		m_Command.m_bActive = true;
		m_Command.m_fMaxClimb = cfg.m_fMaxClimbRate;
		m_Command.m_fMaxDescent = cfg.m_fMaxDescentRate;
		m_Command.m_fArrivalDecel = 2.2;

		vector pos = m_FlightState.m_vPosition;

		if (m_eState == ARIA_EHeliState.TAKEOFF)
		{
			m_Command.m_vTargetPosition = m_vTakeoffPos;
			m_Command.m_fMaxSpeed = 3;
			m_Command.m_bDirectVerticalSpeed = true;
			m_Command.m_fVerticalSpeed = 3.5;
			m_Command.m_eHeadingMode = ARIA_EHeadingMode.HOLD;
			return;
		}

		if (m_eState == ARIA_EHeliState.LANDING)
		{
			float agl = Math.Max(m_Airframe.GetAltitudeAGL(), 0);
			m_Command.m_vTargetPosition = m_vLZ;
			m_Command.m_fMaxSpeed = 4;
			m_Command.m_bDirectVerticalSpeed = true;
			m_Command.m_fVerticalSpeed = -Math.Clamp(agl * 0.35, cfg.m_fLandingDescentRate, cfg.m_fMaxDescentRate * 0.6);
			if (agl < 3)
				m_Command.m_fVerticalSpeed = -cfg.m_fLandingDescentRate * 0.6;

			m_Command.m_eHeadingMode = ARIA_EHeadingMode.HOLD;
			return;
		}

		// FLYING ---------------------------------------------------------------------------------
		bool threatened = IsEnemyNear(pos, 1500);
		float speed = cfg.GetCruiseSpeed();
		float desiredAGL = cfg.m_fCruiseAltitude;
		if (threatened)
		{
			speed = cfg.GetCombatSpeed();
			desiredAGL = cfg.m_fCombatAltitude;
		}

		// Evasive manoeuvre overrides the task
		if (now < m_fEvadeUntil)
		{
			m_Command.m_bVelocityMode = true;
			m_Command.m_vDesiredVelocity = m_vEvadeDir * (cfg.GetCruiseSpeed() * 1.1);
			m_Command.m_fMaxSpeed = cfg.GetCruiseSpeed() * 1.1;
			m_Command.m_fTargetAltitude = ComputeSafeAltitude(now, cfg.m_fNapOfEarthAltitude, m_vEvadeDir, m_Command.m_fMaxSpeed, false);
			return;
		}

		ARIA_HeliTask task = GetCurrentTask(now);
		if (!task)
		{
			HoverAt(now, pos, desiredAGL);
			return;
		}

		if (task.m_fAltitude > 0)
			desiredAGL = task.m_fAltitude;

		switch (task.m_eType)
		{
			case ARIA_EHeliTaskType.MOVE:
			{
				FlyTowards(now, task.m_vPosition, speed, desiredAGL, task.m_bStopAtTarget);
				return;
			}

			case ARIA_EHeliTaskType.LAND:
			case ARIA_EHeliTaskType.RTB:
			{
				vector target = task.m_vPosition;
				if (m_bLZReady)
					target = m_vLZ;

				// Glide slope: come down to approach altitude over the last ~800 m
				float distance = ARIA_Math.HorizontalDistance(pos, target);
				float t = Math.Clamp((distance - 150) / 650, 0, 1);
				float approachAGL = APPROACH_ALTITUDE + (desiredAGL - APPROACH_ALTITUDE) * t;
				FlyTowards(now, target, speed, approachAGL, true);

				// Keep the slow final approach precise
				if (distance < 200)
					m_Command.m_fArrivalDecel = 1.6;
				return;
			}

			case ARIA_EHeliTaskType.ORBIT:
			{
				float radius = task.m_fRadius;
				if (radius <= 0)
					radius = LOITER_RADIUS;

				Orbit(now, task.m_vPosition, radius, desiredAGL, cfg.GetOrbitSpeed());
				return;
			}

			case ARIA_EHeliTaskType.HOVER:
			{
				HoverAt(now, task.m_vPosition, desiredAGL);
				return;
			}
		}

		// BOARD/UNLOAD while airborne: hold position until DecideFlying inserts a LAND task
		HoverAt(now, pos, desiredAGL);
	}

	//------------------------------------------------------------------------------------------------
	protected void FlyTowards(float now, vector target, float speed, float agl, bool stop)
	{
		vector pos = m_FlightState.m_vPosition;
		vector dir = ARIA_Math.SafeNormalize(ARIA_Math.Flatten(target - pos), m_FlightState.GetHeadingDir());

		m_Command.m_fMaxSpeed = speed;
		if (stop)
		{
			m_Command.m_bVelocityMode = false;
			m_Command.m_vTargetPosition = target;
		}
		else
		{
			m_Command.m_bVelocityMode = true;
			m_Command.m_vDesiredVelocity = dir * speed;
		}

		m_Command.m_eHeadingMode = ARIA_EHeadingMode.VELOCITY;
		m_Command.m_fTargetAltitude = ComputeSafeAltitude(now, agl, dir, speed, true);
	}

	//------------------------------------------------------------------------------------------------
	protected void HoverAt(float now, vector point, float agl)
	{
		m_Command.m_bVelocityMode = false;
		m_Command.m_vTargetPosition = point;
		m_Command.m_fMaxSpeed = 15;
		m_Command.m_eHeadingMode = ARIA_EHeadingMode.HOLD;
		m_Command.m_fTargetAltitude = ComputeSafeAltitude(now, agl, m_FlightState.GetHeadingDir(), 5, true);
	}

	//------------------------------------------------------------------------------------------------
	//! Circles `center`; the orbit direction keeps the target on one side for the door gunners.
	protected void Orbit(float now, vector center, float radius, float agl, float speed)
	{
		vector pos = m_FlightState.m_vPosition;
		vector radial = ARIA_Math.Flatten(pos - center);
		float distance = radial.Length();

		// Far away: fly to the orbit entry first
		if (distance > radius * 1.8)
		{
			vector entry = center + ARIA_Math.SafeNormalize(radial, vector.Forward) * radius;
			FlyTowards(now, entry, m_Manager.GetSettings().m_Heli.GetCombatSpeed(), agl, false);
			return;
		}

		vector radialDir = ARIA_Math.SafeNormalize(radial, vector.Forward);
		vector tangent = ARIA_Math.RightOf(radialDir) * m_fOrbitDirection;
		float correction = Math.Clamp((radius - distance) * 0.25, -speed * 0.6, speed * 0.6);
		vector velocity = tangent * speed + radialDir * correction;

		m_Command.m_bVelocityMode = true;
		m_Command.m_vDesiredVelocity = velocity;
		m_Command.m_fMaxSpeed = speed * 1.3;
		m_Command.m_eHeadingMode = ARIA_EHeadingMode.VELOCITY;
		m_Command.m_fTargetAltitude = ComputeSafeAltitude(now, agl, ARIA_Math.SafeNormalize(velocity, tangent), speed, true);
	}

	//------------------------------------------------------------------------------------------------
	//! Absolute altitude that keeps `agl` above terrain AND obstacles along the flight path.
	protected float ComputeSafeAltitude(float now, float agl, vector direction, float speed, bool enforceMinimum)
	{
		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;
		vector pos = m_FlightState.m_vPosition;

		if (now >= m_fNextTerrainSample)
		{
			m_fNextTerrainSample = now + 0.3;
			float ceiling = ARIA_WorldQuery.GetSurfaceY(pos);
			float probeTop = pos[1] + 80;

			// Look-ahead samples along the path (obstacle traces only for the first few hundred meters)
			for (int i = 0; i <= 4; i++)
			{
				float t = cfg.m_fTerrainLookahead * i / 4.0;
				float d = Math.Min(speed * t, 800);
				vector p = pos + direction * d;
				float top = ARIA_WorldQuery.GetSurfaceY(p);
				if (d >= 30 && d <= 350)
					top = Math.Max(top, ARIA_WorldQuery.GetObstacleTopY(p, probeTop, m_Helicopter) + cfg.m_fObstacleClearance - agl * 0.5);

				ceiling = Math.Max(ceiling, top);
			}

			// Straight-ahead collision probe (hills, masts, buildings in the way)
			float probeLength = Math.Clamp(speed * 3 + 30, 40, 250);
			IEntity hit;
			vector probeStart = pos + direction * 10;
			vector probeEnd = pos + direction * probeLength;
			float fraction = ARIA_WorldQuery.TraceSegment(probeStart, probeEnd, m_Helicopter, hit);
			if (fraction < 1)
				ceiling = Math.Max(ceiling, pos[1] + 20);

			m_fTerrainCeiling = ceiling;
		}

		float aglUsed = agl;
		if (enforceMinimum)
			aglUsed = Math.Max(agl, cfg.m_fMinAltitude);

		return m_fTerrainCeiling + aglUsed;
	}

	//==============================================================================================
	// Tasks
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected ARIA_HeliTask GetCurrentTask(float now)
	{
		if (m_aTasks.IsEmpty())
			return null;

		ARIA_HeliTask task = m_aTasks[0];
		if (task.m_fStartTime < 0)
			task.m_fStartTime = now;

		return task;
	}

	//------------------------------------------------------------------------------------------------
	protected void CompleteTask(float now)
	{
		if (m_aTasks.IsEmpty())
			return;

		ARIA_HeliTask task = m_aTasks[0];
		m_aTasks.RemoveOrdered(0);
		ResetLZ();

		if (task.m_bRepeat)
		{
			// Patrol cycles loop forever
			ARIA_HeliTask again = ARIA_HeliTask.Create(task.m_eType, task.m_vPosition);
			again.m_bRepeat = true;
			again.m_bStopAtTarget = task.m_bStopAtTarget;
			again.m_fRadius = task.m_fRadius;
			again.m_fDuration = task.m_fDuration;
			again.m_fAltitude = task.m_fAltitude;
			m_aTasks.Insert(again);
		}

		ARIA_Log.Verbose(GetDebugName() + " task done: " + task.ToDebugString());
	}

	//------------------------------------------------------------------------------------------------
	protected void ClearTasks()
	{
		m_aTasks.Clear();
		ResetLZ();
	}

	//------------------------------------------------------------------------------------------------
	protected void QueueReturnToBase()
	{
		if (m_bHasHome)
		{
			ARIA_HeliTask rtb = ARIA_HeliTask.Create(ARIA_EHeliTaskType.RTB, m_vHome);
			m_aTasks.Insert(rtb);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void StartIdleLoiter()
	{
		// Out of orders while airborne: orbit where we are and wait for new ones
		ARIA_HeliTask loiter = ARIA_HeliTask.Create(ARIA_EHeliTaskType.ORBIT, m_Helicopter.GetOrigin());
		loiter.m_fRadius = LOITER_RADIUS;
		m_aTasks.Insert(loiter);
		m_bIdleLoiter = true;
	}

	//------------------------------------------------------------------------------------------------
	//! Moves the crew group's waypoints into the task queue. The crew group keeps
	//! no waypoints, so the vanilla AI never tries to "walk" the helicopter.
	protected void AbsorbCrewWaypoints()
	{
		if (!m_Crew || !m_Crew.IsValid())
			return;

		SCR_AIGroup group = m_Crew.GetGroup();
		array<AIWaypoint> waypoints = {};
		group.GetWaypoints(waypoints);
		if (waypoints.IsEmpty())
			return;

		if (m_bIdleLoiter)
		{
			ClearTasks();
			m_bIdleLoiter = false;
		}

		for (int i = 0; i < waypoints.Count(); i++)
		{
			bool isLast = i == waypoints.Count() - 1;
			ARIA_HeliWaypointTranslator.Translate(waypoints[i], isLast, HasPassengers(), m_Manager.GetSettings().m_Heli, m_aTasks);
		}

		foreach (AIWaypoint waypoint : waypoints)
		{
			group.RemoveWaypoint(waypoint);
		}

		ARIA_Log.Debug(string.Format("%1 took %2 waypoints, %3 tasks queued", GetDebugName(), waypoints.Count(), m_aTasks.Count()));
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateLZSearch(ARIA_HeliTask task)
	{
		if (m_bLZReady)
			return;

		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;
		if (!m_LZSearch)
		{
			float radius = cfg.m_fLZSearchRadius * (1 + m_iLandingFailures);
			m_LZSearch = new ARIA_LZSearch(task.m_vPosition, radius, cfg.m_fLZClearRadius, cfg.m_fLZMaxSlopeDeg, m_Helicopter);
			m_LZSearch.SetBlacklist(m_aLZBlacklist);
		}

		if (m_LZSearch.Step())
		{
			m_vLZ = m_LZSearch.GetResult();
			m_bLZReady = true;
			if (!m_LZSearch.IsFound())
				ARIA_Log.Warning(GetDebugName() + " found no clear LZ near " + task.m_vPosition.ToString() + ", landing at the best effort spot");
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void ResetLZ()
	{
		m_LZSearch = null;
		m_bLZReady = false;
	}

	//==============================================================================================
	// Passengers
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	//! AI passengers in cargo seats that do not belong to the crew group.
	protected void GetPassengers(notnull array<IEntity> outPassengers)
	{
		array<BaseCompartmentSlot> cargo = {};
		ARIA_VehicleUtils.GetCompartmentsOfType(m_Helicopter, ECompartmentType.CARGO, cargo);
		foreach (BaseCompartmentSlot slot : cargo)
		{
			IEntity occupant = slot.GetOccupant();
			if (!occupant || !ARIA_EntityUtils.IsAbleCharacter(occupant) || ARIA_HeliPilot.IsPlayer(occupant))
				continue;

			AIAgent agent = ARIA_EntityUtils.GetAgent(occupant);
			if (agent && m_Crew && agent.GetParentGroup() == m_Crew.GetGroup())
				continue;

			outPassengers.Insert(occupant);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected bool HasPassengers()
	{
		array<IEntity> passengers = {};
		GetPassengers(passengers);
		return !passengers.IsEmpty();
	}

	//------------------------------------------------------------------------------------------------
	protected void UnloadPassengers(ARIA_GroupBrain squad)
	{
		if (squad && squad.IsValid())
		{
			squad.OrderDisembark(m_Helicopter);
			return;
		}

		// Everybody out (Get Out waypoint): ARIA squads restore their orders, others just leave
		array<IEntity> passengers = {};
		GetPassengers(passengers);
		foreach (IEntity passenger : passengers)
		{
			AIAgent agent = ARIA_EntityUtils.GetAgent(passenger);
			ARIA_GroupBrain brain;
			if (agent)
				brain = m_Manager.FindGroupBrain(agent.GetParentGroup());

			if (brain)
			{
				brain.OrderDisembark(m_Helicopter);
				continue;
			}

			ChimeraCharacter character = ChimeraCharacter.Cast(passenger);
			if (!character)
				continue;

			CompartmentAccessComponent access = character.GetCompartmentAccessComponent();
			if (access)
				access.GetOutVehicle(EGetOutType.ANIMATED, -1, ECloseDoorAfterActions.INVALID, false);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected bool ArePassengersOut(ARIA_GroupBrain squad)
	{
		if (squad && squad.IsValid())
			return squad.IsOutOf(m_Helicopter);

		return !HasPassengers();
	}

	//------------------------------------------------------------------------------------------------
	protected void CancelAirOpsSquads()
	{
		float now = m_Manager.GetTime();
		foreach (ARIA_HeliTask task : m_aTasks)
		{
			if (task.m_bFromAirOps && task.m_Squad && task.m_Squad.IsValid() && task.m_eType == ARIA_EHeliTaskType.UNLOAD)
				task.m_Squad.OnAirliftFinished(now);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void AbortAirOpsMission(float now)
	{
		CancelAirOpsSquads();
		ClearTasks();
		QueueReturnToBase();
		BeginTakeoff(now);
	}

	//==============================================================================================
	// Health, crew, threats
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	//! Returns false (and lets go of the controls) when the helicopter can no longer be flown by ARIA.
	protected bool CheckPilotAndAirframe(float now)
	{
		if (m_Airframe.IsCrippled())
		{
			ARIA_Log.Info(GetDebugName() + " is going down");
			Disable(now);
			return false;
		}

		IEntity pilot = ARIA_VehicleUtils.GetPilot(m_Helicopter);
		if (pilot && IsPlayer(pilot))
		{
			// A player took the controls
			Disable(now);
			return false;
		}

		if (pilot && ARIA_EntityUtils.IsAbleCharacter(pilot))
		{
			m_Pilot = pilot;
			return true;
		}

		if (m_Manager.GetSettings().m_Heli.m_bCopilotTakeover)
		{
			IEntity copilot = ARIA_VehicleUtils.FindCopilot(m_Helicopter, pilot);
			if (copilot && !IsPlayer(copilot))
			{
				if (copilot != m_Pilot)
					ARIA_Log.Info(GetDebugName() + ": co-pilot has the controls");

				m_Pilot = copilot;
				return true;
			}
		}

		ARIA_Log.Info(GetDebugName() + " lost its pilot");
		Disable(now);
		return false;
	}

	//------------------------------------------------------------------------------------------------
	protected void Disable(float now)
	{
		CancelAirOpsSquads();
		m_aTasks.Clear();
		m_Airframe.ReleaseControl();
		SetState(ARIA_EHeliState.DISABLED, now);
	}

	//------------------------------------------------------------------------------------------------
	protected void CheckThreat(float now)
	{
		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;
		float health = m_Airframe.GetHealth();
		bool takingHits = health < m_fLastHealth - 0.02;
		m_fLastHealth = health;

		if (m_eState != ARIA_EHeliState.FLYING)
			return;

		if (takingHits && cfg.m_bEvadeWhenHit && now >= m_fEvadeUntil)
		{
			m_vEvadeDir = ComputeEvadeDirection();
			m_fEvadeUntil = now + 12;
			ARIA_Log.Debug(GetDebugName() + " taking fire, evading");
		}

		if (cfg.m_bRTBWhenDamaged && !m_bDamagedRTB && health <= cfg.m_fRTBHealth)
		{
			m_bDamagedRTB = true;
			ARIA_Log.Info(GetDebugName() + " badly damaged, returning to base");

			bool passengers = HasPassengers();
			CancelAirOpsSquads();
			ClearTasks();
			if (passengers)
			{
				// Drop the passengers at the nearest safe spot first
				ARIA_HeliTask land = ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, m_Helicopter.GetOrigin());
				m_aTasks.Insert(land);
				ARIA_HeliTask unload = ARIA_HeliTask.Create(ARIA_EHeliTaskType.UNLOAD, m_Helicopter.GetOrigin());
				unload.m_fDuration = cfg.m_fUnloadTimeout;
				m_aTasks.Insert(unload);
			}

			QueueReturnToBase();
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Away from the nearest known enemy, or a hard break turn when the shooter is unknown.
	protected vector ComputeEvadeDirection()
	{
		vector pos = m_Helicopter.GetOrigin();
		if (m_Crew)
		{
			ARIA_Contact contact = m_Crew.GetMemory().GetNearest(pos);
			if (contact)
				return ARIA_Math.SafeNormalize(ARIA_Math.Flatten(pos - contact.m_vLastKnownPosition), vector.Forward);
		}

		vector heading = ARIA_Math.Flatten(m_Helicopter.GetWorldTransformAxis(2));
		heading = ARIA_Math.SafeNormalize(heading, vector.Forward);
		vector side = ARIA_Math.RightOf(heading);
		if (Math.RandomFloat(0, 1) < 0.5)
			side = side * -1;

		return ARIA_Math.SafeNormalize(heading + side, heading);
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsEnemyNear(vector pos, float range)
	{
		if (!m_Crew)
			return false;

		ARIA_Contact contact = m_Crew.GetMemory().GetNearest(pos);
		return contact && vector.Distance(contact.m_vLastKnownPosition, pos) < range;
	}

	//------------------------------------------------------------------------------------------------
	static bool IsPlayer(IEntity character)
	{
		PlayerManager playerManager = GetGame().GetPlayerManager();
		if (!playerManager || !character)
			return false;

		return playerManager.GetPlayerIdFromControlledEntity(character) != 0;
	}

	//------------------------------------------------------------------------------------------------
	protected void SetState(ARIA_EHeliState state, float now)
	{
		if (m_eState == state)
			return;

		ARIA_Log.Verbose(string.Format("%1: %2 -> %3", GetDebugName(), typename.EnumToString(ARIA_EHeliState, m_eState), typename.EnumToString(ARIA_EHeliState, state)));
		m_eState = state;
		m_fStateTime = now;
	}
}
