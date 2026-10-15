// ARIA - air operations dispatcher.
//
// Turns idle AI helicopters into on-call support:
//   * Airlift: a squad with a far-away waypoint gets picked up next to its
//     position and dropped (airliftLZOffset) short of its objective.
//   * Close air support: squads in trouble (armour, large enemy forces) get an
//     armed helicopter orbiting the enemy so its door gunners engage.
//
// "Idle" means: an AI-crewed helicopter whose crew group has no waypoints.
// Mission makers simply park a crewed helicopter at a base without waypoints
// and it becomes a support asset for its faction.

class ARIA_CASRequest
{
	vector m_vPosition;
	float m_fTime;
	ARIA_HeliPilot m_Heli;
}

class ARIA_AirOps
{
	protected ARIA_Manager m_Manager;
	protected ref array<ref ARIA_CASRequest> m_aActiveCAS = {};

	protected static const float CAS_DEDUP_RADIUS = 400;

	//------------------------------------------------------------------------------------------------
	void ARIA_AirOps(ARIA_Manager manager)
	{
		m_Manager = manager;
	}

	//------------------------------------------------------------------------------------------------
	//! Returns true when a helicopter accepted the airlift.
	bool RequestAirlift(ARIA_GroupBrain squad, vector destination, float now)
	{
		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;
		if (!cfg.m_bEnabled || !cfg.m_bAutoAirlift)
			return false;

		int seatsNeeded = squad.GetGroup().GetAgentsCount();
		vector squadPos = squad.GetPosition();

		ARIA_HeliPilot best;
		float bestDistSq = cfg.m_fAirliftSearchRadius * cfg.m_fAirliftSearchRadius;
		foreach (ARIA_HeliPilot pilot : m_Manager.GetHeliPilots())
		{
			if (!pilot.IsAvailableForTasking())
				continue;

			if (!ARIA_EntityUtils.AreFriends(pilot.GetFaction(), squad.GetFaction()))
				continue;

			if (pilot.GetFreeCargoSeats() < seatsNeeded)
				continue;

			float d = vector.DistanceSq(pilot.GetHelicopter().GetOrigin(), squadPos);
			if (d < bestDistSq)
			{
				bestDistSq = d;
				best = pilot;
			}
		}

		if (!best)
			return false;

		// Land short of the objective so the helicopter is not shot down over it
		vector dropZone = destination;
		float offset = cfg.m_fAirliftLZOffset;
		vector approach = ARIA_Math.Flatten(squadPos - destination);
		if (offset > 0 && approach.Length() > offset * 1.5)
			dropZone = destination + approach.Normalized() * offset;

		best.AssignAirlift(squad, squadPos, ARIA_WorldQuery.SnapToGround(dropZone));
		squad.OnAirliftAssigned(best, now);
		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Returns true when an armed helicopter was sent.
	bool RequestCAS(ARIA_GroupBrain requester, vector target, float now)
	{
		ARIA_HeliSettings cfg = m_Manager.GetSettings().m_Heli;
		if (!cfg.m_bEnabled || !cfg.m_bAutoCAS)
			return false;

		PruneCAS(now);
		foreach (ARIA_CASRequest active : m_aActiveCAS)
		{
			if (vector.Distance(active.m_vPosition, target) < CAS_DEDUP_RADIUS)
				return false; // somebody is already covering this area
		}

		ARIA_HeliPilot best;
		float bestDistSq = cfg.m_fCASRequestRadius * cfg.m_fCASRequestRadius;
		foreach (ARIA_HeliPilot pilot : m_Manager.GetHeliPilots())
		{
			if (!pilot.IsAvailableForTasking() || !pilot.IsArmed())
				continue;

			if (!ARIA_EntityUtils.AreFriends(pilot.GetFaction(), requester.GetFaction()))
				continue;

			float d = vector.DistanceSq(pilot.GetHelicopter().GetOrigin(), target);
			if (d < bestDistSq)
			{
				bestDistSq = d;
				best = pilot;
			}
		}

		if (!best)
			return false;

		best.AssignCAS(target, cfg.m_fCASStationTime);

		ARIA_CASRequest request = new ARIA_CASRequest();
		request.m_vPosition = target;
		request.m_fTime = now;
		request.m_Heli = best;
		m_aActiveCAS.Insert(request);
		ARIA_Log.Info(requester.GetDebugName() + " receives close air support from " + best.GetDebugName());
		return true;
	}

	//------------------------------------------------------------------------------------------------
	void Update(float now)
	{
	}

	//------------------------------------------------------------------------------------------------
	protected void PruneCAS(float now)
	{
		float stationTime = m_Manager.GetSettings().m_Heli.m_fCASStationTime;
		for (int i = m_aActiveCAS.Count() - 1; i >= 0; i--)
		{
			ARIA_CASRequest request = m_aActiveCAS[i];
			if (!request.m_Heli || !request.m_Heli.IsValid() || now - request.m_fTime > stationTime + 60)
				m_aActiveCAS.Remove(i);
		}
	}
}
