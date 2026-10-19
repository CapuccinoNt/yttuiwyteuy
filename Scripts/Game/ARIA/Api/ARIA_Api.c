// ARIA - public scripting API for scenario makers and other mods.
// All calls are server-side and return false/null when ARIA is not running.
//
// Examples (from a scenario script or a Scenario Framework action):
//
//   ARIA_Api.HeliMove(heliEntity, "4200 0 6100");             // fly there and hover
//   ARIA_Api.HeliLandAndUnload(heliEntity, lzPosition);         // insertion
//   ARIA_Api.HeliOrbit(heliEntity, targetPos, 500, 300);        // 5 min gun run orbit
//   ARIA_Api.RequestAirlift(group, objectivePos);               // taxi a squad
//   ARIA_Api.GetSettings().m_Heli.m_fCruiseSpeedKmh = 200;      // tune at runtime

class ARIA_Api
{
	//------------------------------------------------------------------------------------------------
	static bool IsRunning()
	{
		return ARIA_Manager.GetInstance() != null;
	}

	//------------------------------------------------------------------------------------------------
	//! Live settings object. Call NotifySettingsChanged() after changing controller gains.
	static ARIA_Settings GetSettings()
	{
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (!manager)
			return null;

		return manager.GetSettings();
	}

	//------------------------------------------------------------------------------------------------
	static void NotifySettingsChanged()
	{
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (manager)
			manager.OnSettingsChanged();
	}

	//------------------------------------------------------------------------------------------------
	static ARIA_HeliPilot GetHeliPilot(IEntity helicopter)
	{
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (!manager)
			return null;

		return manager.FindHeliPilot(helicopter);
	}

	//------------------------------------------------------------------------------------------------
	static bool HeliMove(IEntity helicopter, vector position)
	{
		ARIA_HeliPilot pilot = GetHeliPilot(helicopter);
		if (!pilot)
			return false;

		pilot.AddTask(ARIA_HeliTask.Create(ARIA_EHeliTaskType.MOVE, position));
		return true;
	}

	//------------------------------------------------------------------------------------------------
	static bool HeliLandAndUnload(IEntity helicopter, vector position)
	{
		ARIA_HeliPilot pilot = GetHeliPilot(helicopter);
		if (!pilot)
			return false;

		ARIA_HeliSettings cfg = GetSettings().m_Heli;
		pilot.AddTask(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, position));
		ARIA_HeliTask unload = ARIA_HeliTask.Create(ARIA_EHeliTaskType.UNLOAD, position);
		unload.m_fDuration = cfg.m_fUnloadTimeout;
		pilot.AddTask(unload);
		return true;
	}

	//------------------------------------------------------------------------------------------------
	static bool HeliOrbit(IEntity helicopter, vector center, float radius, float durationSeconds)
	{
		ARIA_HeliPilot pilot = GetHeliPilot(helicopter);
		if (!pilot)
			return false;

		ARIA_HeliTask orbit = ARIA_HeliTask.Create(ARIA_EHeliTaskType.ORBIT, center);
		orbit.m_fRadius = radius;
		orbit.m_fDuration = durationSeconds;
		pilot.AddTask(orbit);
		return true;
	}

	//------------------------------------------------------------------------------------------------
	static bool HeliReturnToBase(IEntity helicopter)
	{
		ARIA_HeliPilot pilot = GetHeliPilot(helicopter);
		if (!pilot)
			return false;

		pilot.OrderReturnToBase();
		return true;
	}

	//------------------------------------------------------------------------------------------------
	static bool RequestAirlift(SCR_AIGroup group, vector destination)
	{
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (!manager)
			return false;

		ARIA_GroupBrain brain = manager.FindGroupBrain(group);
		if (!brain)
			return false;

		return manager.GetAirOps().RequestAirlift(brain, destination, manager.GetTime());
	}

	//------------------------------------------------------------------------------------------------
	static bool RequestCAS(SCR_AIGroup requester, vector target)
	{
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (!manager)
			return false;

		ARIA_GroupBrain brain = manager.FindGroupBrain(requester);
		if (!brain)
			return false;

		return manager.GetAirOps().RequestCAS(brain, target, manager.GetTime());
	}

	//------------------------------------------------------------------------------------------------
	//! Reveals an enemy to every squad of `faction` within the radio range around position.
	static void ShareIntel(Faction faction, IEntity enemy, vector position)
	{
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (!manager || !enemy)
			return;

		array<ARIA_GroupBrain> receivers = {};
		manager.FindFriendlyGroups(faction, position, manager.GetSettings().m_Squad.m_fIntelShareRadius, null, receivers);
		foreach (ARIA_GroupBrain receiver : receivers)
		{
			receiver.ReceiveIntel(enemy, position, ARIA_EntityUtils.GetContactType(enemy), manager.GetTime(), manager.GetTime());
		}
	}
}
