// ARIA - ground vehicle brain (cars, trucks, APCs, IFVs, tanks).
//
// The vanilla AI already drives (native AICarMovementComponent). ARIA adds
// what makes vehicles actually useful in a fight:
//
//   * crew roles: passengers jump into empty turrets when contact is made,
//   * dismount drills: transports unload infantry before driving into a
//     kill zone; armed vehicles keep driver + gunner aboard to support,
//   * bail-out when the vehicle is crippled,
//   * convoy discipline: speed cap for multi-vehicle squads,
//   * recovery: stuck vehicles reverse/wiggle out, flipped vehicles are
//     put back on their wheels instead of being abandoned forever.

class ARIA_VehicleBrain
{
	protected ARIA_Manager m_Manager;
	protected IEntity m_Vehicle;
	protected ARIA_GroupBrain m_Owner;

	protected float m_fNextThink;

	// Stuck recovery
	protected vector m_vProgressPos;
	protected float m_fProgressTime;
	protected int m_iRecoveryAttempts;
	protected float m_fRecoveryUntil = -1;
	protected float m_fRecoverySteer;

	// Flip recovery
	protected float m_fUpsideDownSince = -1;

	// Combat crew drills
	protected bool m_bDismounted;
	protected bool m_bBailedOut;
	protected bool m_bConvoyCapped;
	protected bool m_bAbandoned;		//!< recovery gave up: no more shoves for this vehicle

	protected static const int MAX_RECOVERY_ATTEMPTS = 3;
	protected static const float RECOVERY_DURATION = 1.6;

	//------------------------------------------------------------------------------------------------
	void ARIA_VehicleBrain(ARIA_Manager manager, IEntity vehicle, ARIA_GroupBrain owner)
	{
		m_Manager = manager;
		m_Vehicle = vehicle;
		m_Owner = owner;
		m_vProgressPos = vehicle.GetOrigin();
		m_fProgressTime = manager.GetTime();
	}

	//------------------------------------------------------------------------------------------------
	IEntity GetVehicle()
	{
		return m_Vehicle;
	}

	//------------------------------------------------------------------------------------------------
	bool IsValid()
	{
		if (!m_Vehicle || !m_Owner || !m_Owner.IsValid())
			return false;

		if (ARIA_EntityUtils.IsDeadOrDestroyed(m_Vehicle))
			return false;

		// Keep the brain while the squad still uses (or recently dismounted from) the vehicle
		return m_bDismounted || HasOwnerAboard();
	}

	//------------------------------------------------------------------------------------------------
	//! Undo ARIA's changes to the vehicle (convoy speed cap).
	void Release()
	{
		if (!m_bConvoyCapped || !m_Vehicle)
			return;

		AICarMovementComponent movement = AICarMovementComponent.Cast(m_Vehicle.FindComponent(AICarMovementComponent));
		if (movement)
			movement.ResetCruiseSpeed();

		m_bConvoyCapped = false;
	}

	//------------------------------------------------------------------------------------------------
	bool IsThinkDue(float now)
	{
		return now >= m_fNextThink;
	}

	//------------------------------------------------------------------------------------------------
	void Think(float now)
	{
		ARIA_VehicleSettings cfg = m_Manager.GetSettings().m_Vehicles;

		// Recovery manoeuvres need a fast update rate
		if (m_fRecoveryUntil > now)
		{
			ApplyRecoveryImpulse();
			m_fNextThink = now + 0.25;
			return;
		}

		m_fNextThink = now + cfg.m_fThinkInterval;

		UpdateConvoySpeed();

		if (cfg.m_bFlipRecovery)
			UpdateFlipRecovery(now);

		if (cfg.m_bStuckRecovery && !m_bAbandoned)
			UpdateStuckRecovery(now);

		UpdateCombatDrills(now);

		if (cfg.m_bBailOutWhenCrippled)
			UpdateBailOut();
	}

	//==============================================================================================
	// Combat drills
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected void UpdateCombatDrills(float now)
	{
		ARIA_VehicleSettings cfg = m_Manager.GetSettings().m_Vehicles;
		ARIA_ESquadState state = m_Owner.GetState();
		bool inCombat = state != ARIA_ESquadState.IDLE && state != ARIA_ESquadState.REINFORCING && state != ARIA_ESquadState.AIRLIFT;

		if (!inCombat)
		{
			if (m_bDismounted)
				OnCombatOver();

			return;
		}

		float contactDistance = GetNearestContactDistance(now);
		if (contactDistance > cfg.m_fDismountDistance)
			return;

		if (cfg.m_bFillTurrets)
			FillEmptyTurrets();

		if (cfg.m_bDismountOnContact && !m_bDismounted)
			Dismount();
	}

	//------------------------------------------------------------------------------------------------
	//! Passengers move into free turret seats (instant seat switch, like players do).
	protected void FillEmptyTurrets()
	{
		BaseCompartmentSlot turret = ARIA_VehicleUtils.GetFreeSlot(m_Vehicle, ECompartmentType.TURRET);
		if (!turret)
			return;

		array<BaseCompartmentSlot> cargo = {};
		ARIA_VehicleUtils.GetCompartmentsOfType(m_Vehicle, ECompartmentType.CARGO, cargo);
		foreach (BaseCompartmentSlot slot : cargo)
		{
			ChimeraCharacter passenger = ChimeraCharacter.Cast(slot.GetOccupant());
			if (!passenger || !IsOwnerMember(passenger) || !ARIA_EntityUtils.IsAbleCharacter(passenger))
				continue;

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(passenger.GetCompartmentAccessComponent());
			if (!access)
				continue;

			if (access.MoveInVehicle(m_Vehicle, ECompartmentType.TURRET, false, turret))
			{
				ARIA_Log.Debug("Passenger takes the gun of " + ARIA_VehicleUtils.GetDebugName(m_Vehicle));
				return;
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void Dismount()
	{
		ARIA_VehicleSettings cfg = m_Manager.GetSettings().m_Vehicles;
		bool armed = HasOwnerInTurret();
		m_bDismounted = true;

		if (armed && cfg.m_bCrewSupportsDismounts)
		{
			// Only the dismounts get out; driver and gunner keep supporting with the vehicle
			array<BaseCompartmentSlot> cargo = {};
			ARIA_VehicleUtils.GetCompartmentsOfType(m_Vehicle, ECompartmentType.CARGO, cargo);
			int unloaded = 0;
			foreach (BaseCompartmentSlot slot : cargo)
			{
				ChimeraCharacter passenger = ChimeraCharacter.Cast(slot.GetOccupant());
				if (!passenger || !IsOwnerMember(passenger))
					continue;

				CompartmentAccessComponent access = passenger.GetCompartmentAccessComponent();
				if (access && access.GetOutVehicle(EGetOutType.ANIMATED, -1, ECloseDoorAfterActions.INVALID, false))
					unloaded++;
			}

			if (unloaded > 0)
				ARIA_Log.Debug(string.Format("%1 dismounts %2 soldiers, vehicle stays in support", ARIA_VehicleUtils.GetDebugName(m_Vehicle), unloaded));

			return;
		}

		// Unarmed transport: everybody out, fight on foot
		SCR_AIGroupUtilityComponent utility = m_Owner.GetUtility();
		if (!utility)
			return;

		float priority = SCR_AIActionBase.PRIORITY_ACTIVITY_COMBAT_WITH_VEHICLES + 10;
		SCR_AIGetOutActivity getOut = new SCR_AIGetOutActivity(utility, null, m_Vehicle, null, priority, SCR_AIActionBase.PRIORITY_LEVEL_NORMAL);
		utility.AddAction(getOut);
		ARIA_Log.Debug(ARIA_VehicleUtils.GetDebugName(m_Vehicle) + " unloads, squad fights on foot");
	}

	//------------------------------------------------------------------------------------------------
	//! After combat the squad may use the vehicle again for its next move.
	protected void OnCombatOver()
	{
		m_bDismounted = false;
		if (m_bBailedOut)
			return;

		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(m_Vehicle);
		SCR_AIGroupUtilityComponent utility = m_Owner.GetUtility();
		if (usage && utility && ARIA_VehicleUtils.IsDrivable(m_Vehicle, m_Manager.GetSettings().m_Vehicles.m_fBailOutHealth))
			utility.AddUsableVehicle(usage);
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateBailOut()
	{
		if (m_bBailedOut)
			return;

		ARIA_VehicleSettings cfg = m_Manager.GetSettings().m_Vehicles;
		bool crippled = ARIA_EntityUtils.GetHealthScaled(m_Vehicle) <= cfg.m_fBailOutHealth;

		SCR_VehicleDamageManagerComponent damage = SCR_VehicleDamageManagerComponent.Cast(m_Vehicle.FindComponent(SCR_VehicleDamageManagerComponent));
		if (damage && !damage.GetEngineFunctional())
			crippled = true;

		if (!crippled || !HasOwnerAboard())
			return;

		m_bBailedOut = true;
		m_bDismounted = true;

		SCR_AIGroupUtilityComponent utility = m_Owner.GetUtility();
		if (!utility)
			return;

		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(m_Vehicle);
		float priority = SCR_AIActionBase.PRIORITY_ACTIVITY_COMBAT_WITH_VEHICLES + 20;
		SCR_AIGetOutActivity getOut = new SCR_AIGetOutActivity(utility, null, m_Vehicle, null, priority, SCR_AIActionBase.PRIORITY_LEVEL_PLAYER);
		utility.AddAction(getOut);
		if (usage)
			utility.RemoveUsableVehicle(usage);

		ARIA_Log.Debug("Crew bails out of crippled " + ARIA_VehicleUtils.GetDebugName(m_Vehicle));
	}

	//==============================================================================================
	// Convoy
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected void UpdateConvoySpeed()
	{
		AICarMovementComponent movement = AICarMovementComponent.Cast(m_Vehicle.FindComponent(AICarMovementComponent));
		if (!movement)
			return;

		bool convoy = CountOwnerVehicles() > 1;
		if (convoy == m_bConvoyCapped)
			return;

		m_bConvoyCapped = convoy;
		if (convoy)
			movement.SetCruiseSpeed(m_Manager.GetSettings().m_Vehicles.m_fConvoyMaxSpeedKmh);
		else
			movement.ResetCruiseSpeed();
	}

	//==============================================================================================
	// Recovery
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected void UpdateFlipRecovery(float now)
	{
		if (!ARIA_VehicleUtils.IsUpsideDown(m_Vehicle))
		{
			m_fUpsideDownSince = -1;
			return;
		}

		if (m_fUpsideDownSince < 0)
		{
			m_fUpsideDownSince = now;
			return;
		}

		if (now - m_fUpsideDownSince < m_Manager.GetSettings().m_Vehicles.m_fFlipTime)
			return;

		Physics physics = m_Vehicle.GetPhysics();
		if (physics && physics.GetVelocity().Length() > 1.5)
			return;

		Unflip();
		m_fUpsideDownSince = -1;
	}

	//------------------------------------------------------------------------------------------------
	protected void Unflip()
	{
		vector forward = ARIA_Math.Flatten(m_Vehicle.GetWorldTransformAxis(2));
		forward = ARIA_Math.SafeNormalize(forward, vector.Forward);

		vector mat[4];
		mat[0] = ARIA_Math.RightOf(forward);
		mat[1] = vector.Up;
		mat[2] = forward;
		vector pos = m_Vehicle.GetOrigin();
		pos[1] = ARIA_WorldQuery.GetSurfaceY(pos) + 1.5;
		mat[3] = pos;

		BaseGameEntity gameEntity = BaseGameEntity.Cast(m_Vehicle);
		if (gameEntity)
			gameEntity.Teleport(mat);
		else
			m_Vehicle.SetWorldTransform(mat);

		Physics physics = m_Vehicle.GetPhysics();
		if (physics)
		{
			physics.SetVelocity(vector.Zero);
			physics.SetAngularVelocity(vector.Zero);
			physics.SetActive(ActiveState.ACTIVE);
		}

		ARIA_Log.Debug("Flipped vehicle recovered: " + ARIA_VehicleUtils.GetDebugName(m_Vehicle));
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateStuckRecovery(float now)
	{
		ARIA_VehicleSettings cfg = m_Manager.GetSettings().m_Vehicles;

		vector pos = m_Vehicle.GetOrigin();
		if (vector.Distance(pos, m_vProgressPos) > 4)
		{
			m_vProgressPos = pos;
			m_fProgressTime = now;
			m_iRecoveryAttempts = 0;
			return;
		}

		if (!IsTryingToDrive(cfg))
		{
			m_fProgressTime = now;
			return;
		}

		if (now - m_fProgressTime < cfg.m_fStuckTime)
			return;

		m_fProgressTime = now;
		m_iRecoveryAttempts++;

		if (m_iRecoveryAttempts > MAX_RECOVERY_ATTEMPTS)
		{
			// Give up like vanilla does: the squad gets out and walks
			SCR_AIGroupUtilityComponent utility = m_Owner.GetUtility();
			if (utility)
			{
				SCR_AIGetOutActivity getOut = new SCR_AIGetOutActivity(utility, null, m_Vehicle, null, SCR_AIActionBase.PRIORITY_ACTIVITY_GET_OUT + 30, SCR_AIActionBase.PRIORITY_LEVEL_NORMAL);
				utility.AddAction(getOut);
			}

			m_bAbandoned = true;
			m_bDismounted = true;
			m_bBailedOut = true; // never hand this vehicle back after the fight
			ARIA_Log.Debug("Vehicle abandoned after failed recovery: " + ARIA_VehicleUtils.GetDebugName(m_Vehicle));
			return;
		}

		// Reverse with alternating steering: frees most wheel/obstacle snags
		m_fRecoverySteer = 0.35;
		if (m_iRecoveryAttempts % 2 == 0)
			m_fRecoverySteer = -0.35;

		m_fRecoveryUntil = now + RECOVERY_DURATION;
		ARIA_Log.Debug(string.Format("Stuck vehicle recovery %1/%2: %3", m_iRecoveryAttempts, MAX_RECOVERY_ATTEMPTS, ARIA_VehicleUtils.GetDebugName(m_Vehicle)));
	}

	//------------------------------------------------------------------------------------------------
	protected void ApplyRecoveryImpulse()
	{
		Physics physics = m_Vehicle.GetPhysics();
		if (!physics)
			return;

		vector forward = m_Vehicle.GetWorldTransformAxis(2);
		vector velocity = physics.GetVelocity();
		vector reverse = forward * -3.0;
		velocity[0] = reverse[0];
		velocity[2] = reverse[2];
		physics.SetVelocity(velocity);
		physics.SetAngularVelocity(vector.Up * m_fRecoverySteer);
		physics.SetActive(ActiveState.ACTIVE);
	}

	//------------------------------------------------------------------------------------------------
	//! Driver is pressing the throttle but the vehicle does not move.
	protected bool IsTryingToDrive(ARIA_VehicleSettings cfg)
	{
		IEntity driver = ARIA_VehicleUtils.GetPilot(m_Vehicle);
		if (!driver || !IsOwnerMember(driver))
			return false;

		Physics physics = m_Vehicle.GetPhysics();
		if (!physics || physics.GetVelocity().Length() > cfg.m_fStuckSpeed)
			return false;

		// Real throttle input of whatever simulation the vehicle uses; "engine on" is NOT driving
		float throttle = -1;
		VehicleWheeledSimulation wheeled = ARIA_VehicleUtils.GetWheeledSimulation(m_Vehicle);
		if (wheeled)
		{
			throttle = wheeled.GetThrottle();
		}
		else
		{
			VehicleTrackedSimulation tracked = VehicleTrackedSimulation.Cast(m_Vehicle.FindComponent(VehicleTrackedSimulation));
			if (tracked)
			{
				throttle = tracked.GetThrottle();
			}
			else
			{
				VehicleWheeledSimulation_SA_B legacy = VehicleWheeledSimulation_SA_B.Cast(m_Vehicle.FindComponent(VehicleWheeledSimulation_SA_B));
				if (legacy)
					throttle = legacy.GetThrottle();
			}
		}

		return throttle > 0.3;
	}

	//==============================================================================================
	// Helpers
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected float GetNearestContactDistance(float now)
	{
		ARIA_Contact contact = m_Owner.GetMemory().GetNearest(m_Vehicle.GetOrigin());
		if (!contact)
			return float.MAX;

		return vector.Distance(contact.m_vLastKnownPosition, m_Vehicle.GetOrigin());
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsOwnerMember(IEntity character)
	{
		AIAgent agent = ARIA_EntityUtils.GetAgent(character);
		return agent && agent.GetParentGroup() == m_Owner.GetGroup();
	}

	//------------------------------------------------------------------------------------------------
	protected bool HasOwnerAboard()
	{
		array<BaseCompartmentSlot> slots = {};
		ARIA_VehicleUtils.GetAllCompartments(m_Vehicle, slots);
		foreach (BaseCompartmentSlot slot : slots)
		{
			IEntity occupant = slot.GetOccupant();
			if (occupant && IsOwnerMember(occupant))
				return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	protected bool HasOwnerInTurret()
	{
		array<BaseCompartmentSlot> turrets = {};
		ARIA_VehicleUtils.GetCompartmentsOfType(m_Vehicle, ECompartmentType.TURRET, turrets);
		foreach (BaseCompartmentSlot slot : turrets)
		{
			IEntity occupant = slot.GetOccupant();
			if (occupant && IsOwnerMember(occupant) && ARIA_EntityUtils.IsAbleCharacter(occupant))
				return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	protected int CountOwnerVehicles()
	{
		array<IEntity> vehicles = {};
		array<AIAgent> agents = {};
		m_Owner.GetGroup().GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			IEntity vehicle = ARIA_EntityUtils.GetVehicle(agent.GetControlledEntity());
			if (vehicle && !vehicles.Contains(vehicle))
				vehicles.Insert(vehicle);
		}

		return vehicles.Count();
	}
}
