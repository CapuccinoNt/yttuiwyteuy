// ARIA - per-squad tactical brain.
//
// ARIA does not replace the vanilla AI, it commands it. Vanilla soldiers keep
// their own aiming, cover and fire-team logic; ARIA adds the layer the vanilla
// AI lacks: a squad-level decision maker that
//
//   * senses through vanilla group perception and keeps a contact memory,
//   * shares intel by radio with friendly squads (ARIA_Comms),
//   * decides how to fight: engage, flank, assault, hold & suppress, withdraw,
//   * calls for reinforcements / close air support and answers such calls,
//   * uses vehicles (nearby free vehicles, helicopter airlift) for long moves,
//   * applies the configurable skill profile of its faction.
//
// Orders are issued as vanilla group activities (SCR_AIMoveActivity,
// SCR_AISearchAndDestroyActivity, ...) with a priority just above the vanilla
// combat activities, so when an ARIA order ends the squad seamlessly falls
// back to its mission waypoints.

enum ARIA_ESquadState
{
	IDLE,			//!< following mission waypoints, no contact
	ENGAGING,		//!< in contact, vanilla combat logic fights, ARIA monitors
	HOLDING,		//!< outgunned: hold ground and suppress, do not chase
	FLANKING,		//!< moving to a flank staging point
	ASSAULTING,		//!< sweeping the enemy position from the flank
	WITHDRAWING,	//!< breaking contact
	REINFORCING,	//!< moving to help another squad
	AIRLIFT			//!< waiting for / riding in an ARIA helicopter
}

//------------------------------------------------------------------------------------------------
class ARIA_GroupBrain
{
	protected ARIA_Manager m_Manager;
	protected SCR_AIGroup m_Group;
	protected SCR_AIGroupUtilityComponent m_Utility;

	protected ref ARIA_ThreatMemory m_Memory = new ARIA_ThreatMemory();
	protected ARIA_ESquadState m_eState = ARIA_ESquadState.IDLE;
	protected float m_fStateTime;
	protected float m_fNextThink;
	protected float m_fFirstContactTime = -1;
	protected float m_fLastContactTime = -1000;
	protected float m_fLastEvaluation = -1000;
	protected float m_fLastHelpRequest = -1000;
	protected float m_fLastFlankAttempt = -1000;
	protected float m_fLastBroadcast = -1000;
	protected int m_iPeakStrength;
	protected float m_fDefaultAutonomousDistance = -1;

	// Current ARIA order (weak: the vanilla utility component owns the activity)
	protected SCR_AIActivityBase m_Order;
	protected vector m_vOrderTarget;
	protected vector m_vFlankAssaultPos;
	protected ARIA_GroupBrain m_HelpTarget;

	// Assets
	protected ARIA_HeliPilot m_Helicopter;			//!< helicopter this squad crews (pilot squad)
	protected ARIA_HeliPilot m_AirliftHeli;			//!< helicopter transporting this squad
	protected AIWaypoint m_LogisticsWaypoint;		//!< waypoint we already planned transport for
	protected IEntity m_BorrowedVehicle;
	protected IEntity m_StaticWeapon;
	protected float m_fLastStaticCheck = -1000;
	protected ref array<IEntity> m_aSkilledMembers = {};
	protected ref array<AIWaypoint> m_aStashedWaypoints = {};
	protected ref array<vector> m_aStashedPositions = {};

	protected static const float EVALUATION_INTERVAL = 4.0;
	protected static const float HELP_REQUEST_INTERVAL = 45.0;
	protected static const float FLANK_RETRY_INTERVAL = 60.0;
	protected static const float BROADCAST_INTERVAL = 8.0;
	protected static const float ORDER_TIMEOUT = 150.0;
	protected static const float HOLD_DURATION = 40.0;
	protected static const float STATIC_WEAPON_INTERVAL = 30.0;
	protected static const ResourceName MOVE_WAYPOINT = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";

	//------------------------------------------------------------------------------------------------
	void ARIA_GroupBrain(ARIA_Manager manager, SCR_AIGroup group)
	{
		m_Manager = manager;
		m_Group = group;
		m_Utility = group.GetGroupUtilityComponent();
		m_iPeakStrength = group.GetAgentsCount();
		if (m_Utility)
			m_fDefaultAutonomousDistance = m_Utility.GetMaxAutonomousDistance();
	}

	//==============================================================================================
	// Accessors
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	bool IsValid()
	{
		return m_Group && m_Utility && !m_Group.IsDeleted() && m_Group.GetAgentsCount() > 0;
	}

	//------------------------------------------------------------------------------------------------
	SCR_AIGroup GetGroup()
	{
		return m_Group;
	}

	//------------------------------------------------------------------------------------------------
	SCR_AIGroupUtilityComponent GetUtility()
	{
		return m_Utility;
	}

	//------------------------------------------------------------------------------------------------
	Faction GetFaction()
	{
		if (!m_Group)
			return null;

		return m_Group.GetFaction();
	}

	//------------------------------------------------------------------------------------------------
	ARIA_SkillProfile GetSkill()
	{
		return m_Manager.GetSettings().GetSkillProfile(m_Group.GetFactionName());
	}

	//------------------------------------------------------------------------------------------------
	ARIA_ThreatMemory GetMemory()
	{
		return m_Memory;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_ESquadState GetState()
	{
		return m_eState;
	}

	//------------------------------------------------------------------------------------------------
	vector GetPosition()
	{
		IEntity leader = m_Group.GetLeaderEntity();
		if (leader)
			return leader.GetOrigin();

		return m_Group.GetCenterOfMass();
	}

	//------------------------------------------------------------------------------------------------
	string GetDebugName()
	{
		if (!m_Group)
			return "<deleted group>";

		return string.Format("%1 group #%2", m_Group.GetFactionName(), m_Group.GetGroupID());
	}

	//------------------------------------------------------------------------------------------------
	bool IsThinkDue(float now)
	{
		return now >= m_fNextThink;
	}

	//------------------------------------------------------------------------------------------------
	void SetHelicopter(ARIA_HeliPilot pilot)
	{
		m_Helicopter = pilot;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_HeliPilot GetHelicopter()
	{
		return m_Helicopter;
	}

	//------------------------------------------------------------------------------------------------
	void OnSettingsChanged()
	{
		// Re-apply skill to everyone on the next think
		m_aSkilledMembers.Clear();
	}

	//------------------------------------------------------------------------------------------------
	void Release()
	{
		ClearOrder();
		RestoreAutonomousDistance();
		ReleaseStaticWeapon();
		if (m_eState == ARIA_ESquadState.AIRLIFT)
			RestoreWaypoints();
	}

	//==============================================================================================
	// Main loop
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	void Think(float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		m_fNextThink = now + cfg.m_fThinkInterval * Math.RandomFloat(0.85, 1.15);

		ApplySkillToMembers();
		Sense(now);
		m_Memory.Prune(now, cfg.m_fContactMemory);

		int strength = m_Group.GetAgentsCount();
		if (strength > m_iPeakStrength)
			m_iPeakStrength = strength;

		// Crew squads of helicopters are driven by their ARIA_HeliPilot
		if (m_Helicopter && m_Helicopter.IsValid())
			return;

		if (m_eState == ARIA_ESquadState.AIRLIFT)
		{
			UpdateAirlift(now);
			return;
		}

		// Mission makers / Game Master forced orders always win
		if (IsUnderDirectOrders())
		{
			if (m_eState != ARIA_ESquadState.IDLE)
				EndCombat(now, false);

			return;
		}

		UpdateState(now);

		if (m_eState == ARIA_ESquadState.IDLE)
			UpdateLogistics(now);
	}

	//------------------------------------------------------------------------------------------------
	//! Reads vanilla group perception into ARIA memory and radios new contacts.
	protected void Sense(float now)
	{
		SCR_AIGroupPerception perception = m_Utility.m_Perception;
		if (!perception)
			return;

		PerceptionManager perceptionManager = GetGame().GetPerceptionManager();
		float perceptionTime = 0;
		if (perceptionManager)
			perceptionTime = perceptionManager.GetTime();

		Faction ownFaction = GetFaction();
		ARIA_Contact newest;

		foreach (SCR_AITargetInfo target : perception.m_aTargets)
		{
			if (!target || !target.m_Entity)
				continue;

			if (target.m_eCategory == EAITargetInfoCategory.DESTROYED || target.m_eCategory == EAITargetInfoCategory.DISARMED)
				continue;

			if (target.m_Faction && ownFaction && !ownFaction.IsFactionEnemy(target.m_Faction))
				continue;

			float age = 0;
			if (perceptionManager)
				age = Math.Max(0, perceptionTime - target.m_fTimestamp);

			ARIA_EContactType type = ARIA_EntityUtils.GetContactType(target.m_Entity);
			bool isNew = m_Memory.Report(target.m_Entity, target.m_vWorldPos, type, now - age, false);
			if (isNew)
				newest = m_Memory.Find(target.m_Entity);
		}

		// Track contact timing
		ARIA_Contact recent = m_Memory.GetMostRecent();
		if (recent && recent.GetAge(now) < 5)
			m_fLastContactTime = Math.Max(m_fLastContactTime, recent.m_fLastSeenTime);

		if (newest && now - m_fLastBroadcast >= BROADCAST_INTERVAL)
		{
			m_fLastBroadcast = now;
			m_Manager.GetComms().BroadcastContact(this, newest, now);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Radio report from another squad (ARIA_Comms).
	void ReceiveIntel(IEntity enemy, vector position, ARIA_EContactType type, float now)
	{
		m_Memory.Report(enemy, position, type, now, true);

		// Feed the vanilla group perception so native AI reacts on its own
		SCR_AIGroupPerception perception = m_Utility.m_Perception;
		PerceptionManager perceptionManager = GetGame().GetPerceptionManager();
		if (perception && perceptionManager && enemy)
			perception.AddOrUpdateGunshot(enemy, position, ARIA_EntityUtils.GetFaction(enemy), perceptionManager.GetTime(), false);
	}

	//==============================================================================================
	// Combat state machine
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected void UpdateState(float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		bool inContact = HasActiveContact(now);

		switch (m_eState)
		{
			case ARIA_ESquadState.IDLE:
			{
				if (!inContact)
				{
					m_fFirstContactTime = -1;
					break;
				}

				if (m_fFirstContactTime < 0)
					m_fFirstContactTime = now;

				// Reaction time from the skill profile
				if (now - m_fFirstContactTime >= GetSkill().m_fReactionTime)
				{
					SetState(ARIA_ESquadState.ENGAGING, now);
					EvaluateCombat(now);
				}
				break;
			}

			case ARIA_ESquadState.ENGAGING:
			case ARIA_ESquadState.HOLDING:
			{
				if (now - m_fLastContactTime > cfg.m_fCombatTimeout && !inContact)
				{
					EndCombat(now, true);
					break;
				}

				if (m_eState == ARIA_ESquadState.HOLDING && now - m_fStateTime > HOLD_DURATION)
				{
					RestoreAutonomousDistance();
					SetState(ARIA_ESquadState.ENGAGING, now);
				}

				if (now - m_fLastEvaluation >= EVALUATION_INTERVAL)
					EvaluateCombat(now);
				break;
			}

			case ARIA_ESquadState.FLANKING:
			{
				if (CheckWithdraw(now))
					break;

				if (IsOrderFinished() || IsOrderTimedOut(now))
				{
					if (IsOrderSucceeded())
						StartAssault(now);
					else
						SetState(ARIA_ESquadState.ENGAGING, now);
				}
				break;
			}

			case ARIA_ESquadState.ASSAULTING:
			{
				if (CheckWithdraw(now))
					break;

				if (IsOrderFinished() || IsOrderTimedOut(now))
				{
					ClearOrder();
					if (inContact)
						SetState(ARIA_ESquadState.ENGAGING, now);
					else
						EndCombat(now, true);
				}
				break;
			}

			case ARIA_ESquadState.WITHDRAWING:
			{
				if (IsOrderFinished() || IsOrderTimedOut(now))
				{
					ClearOrder();
					// Rally and hold at the fallback position
					SetHoldPosition();
					SetState(ARIA_ESquadState.HOLDING, now);
				}
				break;
			}

			case ARIA_ESquadState.REINFORCING:
			{
				bool targetGone = !m_HelpTarget || !m_HelpTarget.IsValid();
				if (IsOrderFinished() || IsOrderTimedOut(now) || targetGone)
				{
					ClearOrder();
					m_HelpTarget = null;
					if (inContact)
						SetState(ARIA_ESquadState.ENGAGING, now);
					else
						EndCombat(now, true);
				}
				break;
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	//! The heart of the squad AI: decides how to fight the current contacts.
	protected void EvaluateCombat(float now)
	{
		m_fLastEvaluation = now;
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		ARIA_SkillProfile skill = GetSkill();

		vector pos = GetPosition();
		float own = GetOwnStrength();
		float enemy = m_Memory.GetThreatStrength(pos, cfg.m_fEngageRange);
		float ratio = enemy / Math.Max(own, 0.5);

		// 1. Morale: break contact when the fight is lost
		if (CheckWithdraw(now))
			return;

		// 2. Ask for help when outgunned (or when facing armour / aircraft)
		bool heavyThreat = m_Memory.HasContactOfType(ARIA_EContactType.ARMOR) || m_Memory.HasContactOfType(ARIA_EContactType.AIR);
		if ((ratio >= 1.0 || heavyThreat) && now - m_fLastHelpRequest >= HELP_REQUEST_INTERVAL)
		{
			m_fLastHelpRequest = now;
			vector enemyPos = pos;
			m_Memory.GetEnemyCentroid(pos, cfg.m_fEngageRange, enemyPos);
			m_Manager.GetComms().RequestHelp(this, enemyPos, enemy, now);
		}

		// 3. Firepower: put a gunner on a free static weapon nearby
		TryManStaticWeapon(now);

		// 4. Outgunned but not broken: hold and suppress instead of trickling forward
		if (cfg.m_bSuppressAndHold && ratio > 1.3)
		{
			if (m_eState != ARIA_ESquadState.HOLDING)
			{
				SetHoldPosition();
				SetState(ARIA_ESquadState.HOLDING, now);
				ARIA_Log.Debug(GetDebugName() + " outgunned, holding and suppressing");
			}
			return;
		}

		// 5. Initiative: flank when we have the numbers and the guts
		if (cfg.m_bFlanking && m_eState == ARIA_ESquadState.ENGAGING && now - m_fLastFlankAttempt >= FLANK_RETRY_INTERVAL)
		{
			m_fLastFlankAttempt = now;
			if (own >= 3 && ratio <= 1.5 && !IsInAnyVehicle() && ARIA_Math.Chance(0.2 + 0.6 * skill.m_fAggression))
				TryStartFlank(now);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Returns true when the squad started withdrawing.
	protected bool CheckWithdraw(float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		if (!cfg.m_bWithdraw || m_eState == ARIA_ESquadState.WITHDRAWING)
			return false;

		vector pos = GetPosition();
		float own = GetOwnStrength();
		float enemy = m_Memory.GetThreatStrength(pos, cfg.m_fEngageRange);
		float ratio = enemy / Math.Max(own, 0.5);
		float casualties = 1 - (m_Group.GetAgentsCount() / Math.Max(1.0, m_iPeakStrength));

		float courage = 0.5 + GetSkill().m_fCourage; // 0.5 .. 1.5
		bool broken = casualties >= cfg.m_fWithdrawCasualtyRatio * courage && ratio > 0.8;
		bool overwhelmed = ratio >= cfg.m_fWithdrawForceRatio * courage;
		if (!broken && !overwhelmed)
			return false;

		vector enemyPos;
		if (!m_Memory.GetEnemyCentroid(pos, cfg.m_fEngageRange, enemyPos))
			return false;

		vector fallback = ARIA_TacticsPlanner.PlanWithdraw(pos, enemyPos, cfg.m_fWithdrawDistance);
		if (!IssueMove(fallback, true, EMovementType.SPRINT, true))
			return false;

		SetState(ARIA_ESquadState.WITHDRAWING, now);
		ARIA_Log.Debug(string.Format("%1 withdrawing (casualties %2, force ratio %3)", GetDebugName(), casualties, ratio));

		if (now - m_fLastHelpRequest >= HELP_REQUEST_INTERVAL * 0.5)
		{
			m_fLastHelpRequest = now;
			m_Manager.GetComms().RequestHelp(this, enemyPos, enemy, now);
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected void TryStartFlank(float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		vector pos = GetPosition();
		ARIA_Contact target = m_Memory.GetNearest(pos);
		if (!target)
			return;

		vector flankPos;
		vector stagingPos;
		if (!ARIA_TacticsPlanner.PlanFlank(pos, target.m_vLastKnownPosition, cfg.m_fFlankMinDistance, cfg.m_fFlankMaxDistance, flankPos, stagingPos))
			return;

		// Concealed route: move with priority over combat so the squad does not stall in the open
		if (!IssueMove(stagingPos, true, EMovementType.RUN, false))
			return;

		m_vFlankAssaultPos = flankPos;
		SetState(ARIA_ESquadState.FLANKING, now);
		ARIA_Log.Debug(GetDebugName() + " flanking via " + stagingPos.ToString());
	}

	//------------------------------------------------------------------------------------------------
	protected void StartAssault(float now)
	{
		// Prefer the freshest enemy position, fall back to the planned assault position
		vector target = m_vFlankAssaultPos;
		ARIA_Contact contact = m_Memory.GetNearest(m_vFlankAssaultPos);
		if (contact)
			target = contact.m_vLastKnownPosition;

		if (IssueSearchAndDestroy(target))
			SetState(ARIA_ESquadState.ASSAULTING, now);
		else
			SetState(ARIA_ESquadState.ENGAGING, now);
	}

	//------------------------------------------------------------------------------------------------
	protected void EndCombat(float now, bool regroup)
	{
		ClearOrder();
		RestoreAutonomousDistance();
		ReleaseStaticWeapon();
		m_HelpTarget = null;
		m_fFirstContactTime = -1;
		SetState(ARIA_ESquadState.IDLE, now);
		if (regroup)
			ARIA_Log.Debug(GetDebugName() + " combat over, resuming orders");
	}

	//------------------------------------------------------------------------------------------------
	protected void SetState(ARIA_ESquadState state, float now)
	{
		if (m_eState == state)
			return;

		ARIA_Log.Verbose(string.Format("%1: %2 -> %3", GetDebugName(), typename.EnumToString(ARIA_ESquadState, m_eState), typename.EnumToString(ARIA_ESquadState, state)));
		m_eState = state;
		m_fStateTime = now;
	}

	//------------------------------------------------------------------------------------------------
	protected void TryManStaticWeapon(float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		if (!cfg.m_bUseStaticWeapons || m_StaticWeapon || now - m_fLastStaticCheck < STATIC_WEAPON_INTERVAL)
			return;

		m_fLastStaticCheck = now;
		if (m_Group.GetAgentsCount() < 2 || IsInAnyVehicle())
			return;

		IEntity weapon = ARIA_VehicleUtils.FindFreeStaticWeapon(GetPosition(), cfg.m_fStaticWeaponRadius, GetFaction());
		if (!weapon)
			return;

		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(weapon);
		if (!usage)
		{
			ARIA_VehicleUtils.Unclaim(weapon);
			return;
		}

		SCR_AIBoardingParameters boarding = new SCR_AIBoardingParameters();
		boarding.m_bIsDriverAllowed = false;
		boarding.m_bIsGunnerAllowed = true;
		boarding.m_bIsCargoAllowed = false;

		m_Utility.AddUsableVehicle(usage);
		float priority = SCR_AIActionBase.PRIORITY_ACTIVITY_COMBAT_WITH_VEHICLES + 8;
		SCR_AIGetInActivity getIn = new SCR_AIGetInActivity(m_Utility, null, weapon, boarding, ECompartmentType.TURRET, priority, SCR_AIActionBase.PRIORITY_LEVEL_NORMAL);
		m_Utility.AddAction(getIn);
		m_StaticWeapon = weapon;
		ARIA_Log.Debug(GetDebugName() + " mans static weapon " + ARIA_VehicleUtils.GetDebugName(weapon));
	}

	//------------------------------------------------------------------------------------------------
	//! After the fight the gunner leaves the static weapon and rejoins the squad.
	protected void ReleaseStaticWeapon()
	{
		if (!m_StaticWeapon)
			return;

		if (m_Utility && !ARIA_EntityUtils.IsDeadOrDestroyed(m_StaticWeapon))
		{
			SCR_AIGetOutActivity getOut = new SCR_AIGetOutActivity(m_Utility, null, m_StaticWeapon, null, SCR_AIActionBase.PRIORITY_ACTIVITY_GET_OUT, SCR_AIActionBase.PRIORITY_LEVEL_NORMAL);
			m_Utility.AddAction(getOut);
		}

		ARIA_VehicleUtils.Unclaim(m_StaticWeapon);
		m_StaticWeapon = null;
	}

	//==============================================================================================
	// Cooperation
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	//! Idle, healthy, not under direct orders.
	bool CanReinforce()
	{
		if (!IsValid() || m_eState != ARIA_ESquadState.IDLE)
			return false;

		if (m_Helicopter || IsUnderDirectOrders())
			return false;

		return m_Group.GetAgentsCount() >= 2;
	}

	//------------------------------------------------------------------------------------------------
	bool OrderReinforce(ARIA_GroupBrain requester, vector enemyPosition, float now)
	{
		vector point = ARIA_TacticsPlanner.PlanReinforcementPoint(GetPosition(), requester.GetPosition(), enemyPosition);
		float distance = vector.Distance(GetPosition(), point);
		bool useVehicles = distance > m_Manager.GetSettings().m_Squad.m_fMinTravelForVehicle;
		if (!IssueMove(point, false, EMovementType.RUN, useVehicles))
			return false;

		m_HelpTarget = requester;
		SetState(ARIA_ESquadState.REINFORCING, now);
		ARIA_Log.Debug(GetDebugName() + " reinforcing " + requester.GetDebugName());
		return true;
	}

	//==============================================================================================
	// Logistics: vehicles and airlift
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected void UpdateLogistics(float now)
	{
		AIWaypoint waypoint = m_Group.GetCurrentWaypoint();
		if (!waypoint || waypoint == m_LogisticsWaypoint)
			return;

		if (IsInAnyVehicle())
			return;

		m_LogisticsWaypoint = waypoint;
		float distance = vector.Distance(GetPosition(), waypoint.GetOrigin());

		ARIA_HeliSettings heli = m_Manager.GetSettings().m_Heli;
		if (heli.m_bEnabled && heli.m_bAutoAirlift && distance >= heli.m_fAirliftMinDistance && !IsEditorControlled(waypoint))
		{
			if (m_Manager.GetAirOps().RequestAirlift(this, waypoint.GetOrigin(), now))
				return;
		}

		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		if (cfg.m_bUseNearbyVehicles && distance >= cfg.m_fMinTravelForVehicle)
			TryBorrowVehicle();
	}

	//------------------------------------------------------------------------------------------------
	//! Finds a free friendly vehicle with enough seats and lets the vanilla AI drive it.
	protected void TryBorrowVehicle()
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		int seatsNeeded = m_Group.GetAgentsCount();
		IEntity vehicle = ARIA_VehicleUtils.FindFreeVehicle(GetPosition(), cfg.m_fVehicleSearchRadius, GetFaction(), seatsNeeded);
		if (!vehicle)
			return;

		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(vehicle);
		if (!usage)
			return;

		m_Utility.AddUsableVehicle(usage);
		SCR_AIGetInActivity getIn = new SCR_AIGetInActivity(m_Utility, null, vehicle, null, ECompartmentType.CARGO, SCR_AIActionBase.PRIORITY_ACTIVITY_GET_IN, SCR_AIActionBase.PRIORITY_LEVEL_NORMAL);
		m_Utility.AddAction(getIn);
		m_BorrowedVehicle = vehicle;
		ARIA_Log.Debug(GetDebugName() + " mounting up in " + ARIA_VehicleUtils.GetDebugName(vehicle));
	}

	//------------------------------------------------------------------------------------------------
	//! Called by ARIA_AirOps when a helicopter accepted to transport this squad.
	void OnAirliftAssigned(ARIA_HeliPilot heli, float now)
	{
		m_AirliftHeli = heli;
		ClearOrder();
		StashWaypoints();
		SetState(ARIA_ESquadState.AIRLIFT, now);
		ARIA_Log.Info(GetDebugName() + " waiting for airlift by " + heli.GetDebugName());
	}

	//------------------------------------------------------------------------------------------------
	//! Helicopter landed next to us: board it (cargo seats only).
	void OrderBoard(IEntity helicopter)
	{
		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(helicopter);
		if (usage)
			m_Utility.AddUsableVehicle(usage);

		SCR_AIBoardingParameters boarding = new SCR_AIBoardingParameters();
		boarding.m_bIsDriverAllowed = false;
		boarding.m_bIsGunnerAllowed = false;
		boarding.m_bIsCargoAllowed = true;

		SCR_AIGetInActivity getIn = new SCR_AIGetInActivity(m_Utility, null, helicopter, boarding, ECompartmentType.CARGO, SCR_AIActionBase.PRIORITY_ACTIVITY_GET_IN, SCR_AIActionBase.PRIORITY_LEVEL_PLAYER);
		m_Utility.AddAction(getIn);
		m_Order = getIn;
	}

	//------------------------------------------------------------------------------------------------
	//! True when every able member sits in the given vehicle.
	bool IsBoarded(IEntity vehicle)
	{
		array<AIAgent> agents = {};
		m_Group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			IEntity character = agent.GetControlledEntity();
			if (!ARIA_EntityUtils.IsAbleCharacter(character))
				continue;

			if (ARIA_EntityUtils.GetVehicle(character) != vehicle)
				return false;
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Unloads the squad from the helicopter and restores its mission waypoints.
	void OrderDisembark(IEntity helicopter)
	{
		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(helicopter);
		if (usage)
			m_Utility.RemoveUsableVehicle(usage);

		array<AIAgent> agents = {};
		m_Group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			ChimeraCharacter character = ChimeraCharacter.Cast(agent.GetControlledEntity());
			if (!character || ARIA_EntityUtils.GetVehicle(character) != helicopter)
				continue;

			CompartmentAccessComponent access = character.GetCompartmentAccessComponent();
			if (access)
				access.GetOutVehicle(EGetOutType.ANIMATED, -1, ECloseDoorAfterActions.INVALID, false);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! True when nobody of the squad is left inside the vehicle.
	bool IsOutOf(IEntity vehicle)
	{
		array<AIAgent> agents = {};
		m_Group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			if (ARIA_EntityUtils.GetVehicle(agent.GetControlledEntity()) == vehicle)
				return false;
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Airlift finished (or was cancelled): resume normal operations.
	void OnAirliftFinished(float now)
	{
		m_AirliftHeli = null;
		ClearOrder();
		RestoreWaypoints();
		SetState(ARIA_ESquadState.IDLE, now);
		m_LogisticsWaypoint = m_Group.GetCurrentWaypoint();
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateAirlift(float now)
	{
		// The helicopter drives the sequence; we only watch for it disappearing
		if (!m_AirliftHeli || !m_AirliftHeli.IsValid())
		{
			ARIA_Log.Debug(GetDebugName() + " airlift cancelled");
			OnAirliftFinished(now);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Destination of the squad (first stashed or current waypoint).
	bool GetDestination(out vector destination)
	{
		if (!m_aStashedPositions.IsEmpty())
		{
			destination = m_aStashedPositions[0];
			return true;
		}

		AIWaypoint waypoint = m_Group.GetCurrentWaypoint();
		if (!waypoint)
			return false;

		destination = waypoint.GetOrigin();
		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Removes the waypoints while flying so the vanilla AI does not try to walk from the cabin.
	protected void StashWaypoints()
	{
		m_aStashedWaypoints.Clear();
		m_aStashedPositions.Clear();

		array<AIWaypoint> waypoints = {};
		m_Group.GetWaypoints(waypoints);
		foreach (AIWaypoint waypoint : waypoints)
		{
			m_aStashedWaypoints.Insert(waypoint);
			m_aStashedPositions.Insert(waypoint.GetOrigin());
		}

		foreach (AIWaypoint waypoint : waypoints)
		{
			m_Group.RemoveWaypoint(waypoint);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void RestoreWaypoints()
	{
		for (int i = 0; i < m_aStashedPositions.Count(); i++)
		{
			AIWaypoint waypoint;
			if (i < m_aStashedWaypoints.Count())
				waypoint = m_aStashedWaypoints[i];

			// The waypoint entity may have been deleted by its owner meanwhile: recreate a move
			if (!waypoint)
				waypoint = SpawnMoveWaypoint(m_aStashedPositions[i]);

			if (waypoint)
				m_Group.AddWaypoint(waypoint);
		}

		m_aStashedWaypoints.Clear();
		m_aStashedPositions.Clear();
	}

	//------------------------------------------------------------------------------------------------
	protected AIWaypoint SpawnMoveWaypoint(vector position)
	{
		Resource resource = Resource.Load(MOVE_WAYPOINT);
		if (!resource || !resource.IsValid())
			return null;

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = position;
		return AIWaypoint.Cast(GetGame().SpawnEntityPrefabLocal(resource, null, params));
	}

	//==============================================================================================
	// Orders (vanilla activities)
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	//! \param forced true = ignore enemies while moving (withdraw, concealed flank route)
	protected bool IssueMove(vector position, bool forced, EMovementType movement, bool useVehicles)
	{
		ClearOrder();

		float priority = SCR_AIActionBase.PRIORITY_ACTIVITY_COMBAT_WITH_VEHICLES + 5;
		float level = SCR_AIActionBase.PRIORITY_LEVEL_NORMAL;
		if (forced)
			level = SCR_AIActionBase.PRIORITY_LEVEL_PLAYER;

		SCR_AIMoveActivity activity = new SCR_AIMoveActivity(m_Utility, null, position, null, movement, useVehicles, priority, level);
		m_Utility.AddAction(activity);
		m_Order = activity;
		m_vOrderTarget = position;
		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IssueSearchAndDestroy(vector position)
	{
		ClearOrder();

		float priority = SCR_AIActionBase.PRIORITY_ACTIVITY_COMBAT_WITH_VEHICLES + 5;
		SCR_AISearchAndDestroyActivity activity = new SCR_AISearchAndDestroyActivity(m_Utility, null, position, null, EMovementType.RUN, false, priority, SCR_AIActionBase.PRIORITY_LEVEL_NORMAL);
		m_Utility.AddAction(activity);
		m_Order = activity;
		m_vOrderTarget = position;
		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected void ClearOrder()
	{
		if (m_Order)
		{
			EAIActionState state = m_Order.GetActionState();
			if (state != EAIActionState.COMPLETED && state != EAIActionState.FAILED)
				m_Order.SetActionState(EAIActionState.FAILED);
		}

		m_Order = null;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsOrderFinished()
	{
		if (!m_Order)
			return true;

		EAIActionState state = m_Order.GetActionState();
		return state == EAIActionState.COMPLETED || state == EAIActionState.FAILED;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsOrderSucceeded()
	{
		if (m_Order)
			return m_Order.GetActionState() == EAIActionState.COMPLETED;

		// Activity already cleaned up by vanilla: judge by distance to the target
		return vector.Distance(GetPosition(), m_vOrderTarget) < 40;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsOrderTimedOut(float now)
	{
		return now - m_fStateTime > ORDER_TIMEOUT;
	}

	//------------------------------------------------------------------------------------------------
	//! Keeps members close instead of chasing contacts (hold & suppress).
	protected void SetHoldPosition()
	{
		m_Utility.SetMaxAutonomousDistance(40);
	}

	//------------------------------------------------------------------------------------------------
	protected void RestoreAutonomousDistance()
	{
		if (m_Utility && m_fDefaultAutonomousDistance > 0)
			m_Utility.SetMaxAutonomousDistance(m_fDefaultAutonomousDistance);
	}

	//==============================================================================================
	// Helpers
	//==============================================================================================

	//------------------------------------------------------------------------------------------------
	protected bool HasActiveContact(float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		vector pos = GetPosition();
		float rangeSq = cfg.m_fEngageRange * cfg.m_fEngageRange;
		foreach (ARIA_Contact contact : m_Memory.GetContacts())
		{
			if (contact.GetAge(now) > cfg.m_fCombatTimeout)
				continue;

			if (vector.DistanceSq(pos, contact.m_vLastKnownPosition) <= rangeSq)
				return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Combat power: able soldiers plus the vehicles they crew.
	float GetOwnStrength()
	{
		float strength = 0;
		array<IEntity> countedVehicles = {};
		array<AIAgent> agents = {};
		m_Group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			IEntity character = agent.GetControlledEntity();
			if (!ARIA_EntityUtils.IsAbleCharacter(character))
				continue;

			strength += 1;
			IEntity vehicle = ARIA_EntityUtils.GetVehicle(character);
			if (vehicle && !countedVehicles.Contains(vehicle))
			{
				countedVehicles.Insert(vehicle);
				strength += ARIA_VehicleUtils.GetCombatValue(vehicle);
			}
		}

		return strength;
	}

	//------------------------------------------------------------------------------------------------
	bool IsInAnyVehicle()
	{
		array<AIAgent> agents = {};
		m_Group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			if (ARIA_EntityUtils.GetVehicle(agent.GetControlledEntity()))
				return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Forced Game Master / scenario waypoints (priority level > 0) are never overridden.
	protected bool IsUnderDirectOrders()
	{
		SCR_AIWaypoint waypoint = SCR_AIWaypoint.Cast(m_Group.GetCurrentWaypoint());
		if (!waypoint)
			return false;

		return waypoint.GetPriorityLevel() > SCR_AIActionBase.PRIORITY_LEVEL_NORMAL;
	}

	//------------------------------------------------------------------------------------------------
	//! Waypoints placed by a Game Master are not stashed/airlifted automatically.
	protected bool IsEditorControlled(AIWaypoint waypoint)
	{
		return waypoint && waypoint.FindComponent(SCR_EditableEntityComponent) != null;
	}

	//------------------------------------------------------------------------------------------------
	//! Applies the faction skill profile to members that do not have it yet.
	protected void ApplySkillToMembers()
	{
		for (int i = m_aSkilledMembers.Count() - 1; i >= 0; i--)
		{
			if (!m_aSkilledMembers[i])
				m_aSkilledMembers.Remove(i);
		}

		ARIA_SkillProfile skill = GetSkill();
		array<AIAgent> agents = {};
		m_Group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			IEntity character = agent.GetControlledEntity();
			if (!character || m_aSkilledMembers.Contains(character))
				continue;

			SCR_AICombatComponent combat = SCR_AICombatComponent.Cast(character.FindComponent(SCR_AICombatComponent));
			if (!combat)
				continue;

			combat.SetAISkill(ARIA_SkillMapper.ToVanillaSkill(skill.m_fSkill));
			combat.SetPerceptionFactor(skill.m_fPerception);
			m_aSkilledMembers.Insert(character);
		}
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_SkillMapper
{
	//! Maps ARIA skill [0..1] to the vanilla EAISkill enum (which drives aim error).
	static EAISkill ToVanillaSkill(float skill)
	{
		if (skill < 0.15)
			return EAISkill.NOOB;

		if (skill < 0.35)
			return EAISkill.ROOKIE;

		if (skill < 0.6)
			return EAISkill.REGULAR;

		if (skill < 0.8)
			return EAISkill.VETERAN;

		if (skill < 0.97)
			return EAISkill.EXPERT;

		return EAISkill.CYLON;
	}
}
