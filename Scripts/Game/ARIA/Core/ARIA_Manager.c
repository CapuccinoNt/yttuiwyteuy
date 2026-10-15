// ARIA - central manager. Server/authority only.
//
// Owns every "brain":
//   ARIA_GroupBrain    one per AI group   (squad tactics, intel, vehicle usage)
//   ARIA_VehicleBrain  one per crewed ground vehicle (crew roles, recovery)
//   ARIA_HeliPilot     one per AI-piloted helicopter (autopilot + missions)
//
// Ticked every frame from the game mode (see ARIA_GameModeHook.c). Squads and
// vehicles are time-sliced (a few per frame); helicopters need a per-frame
// update because they are flown by the flight controller.

class ARIA_Manager
{
	protected static ref ARIA_Manager s_Instance;

	protected ref ARIA_Settings m_Settings;
	protected ref ARIA_Comms m_Comms;
	protected ref ARIA_AirOps m_AirOps;

	protected ref array<ref ARIA_GroupBrain> m_aGroups = {};
	protected ref array<ref ARIA_VehicleBrain> m_aVehicles = {};
	protected ref array<ref ARIA_HeliPilot> m_aHelis = {};

	protected float m_fTime;
	protected float m_fDiscoveryTimer;
	protected int m_iGroupCursor;
	protected int m_iVehicleCursor;

	protected static const float DISCOVERY_INTERVAL = 3.0;

	//------------------------------------------------------------------------------------------------
	//! Creates the manager. Call on the server only.
	static void Start()
	{
		if (s_Instance)
			return;

		if (Replication.IsRunning() && !Replication.IsServer())
			return;

		ARIA_Settings settings = ARIA_SettingsIO.Load();
		ARIA_Log.s_eLevel = settings.m_iLogLevel;
		if (!settings.m_bEnabled)
		{
			ARIA_Log.Info("Disabled in settings (general.enabled = false)");
			return;
		}

		s_Instance = new ARIA_Manager(settings);
		ARIA_Log.Info(string.Format("ARIA started - squads:%1 vehicles:%2 helicopters:%3", settings.m_Squad.m_bEnabled, settings.m_Vehicles.m_bEnabled, settings.m_Heli.m_bEnabled));
	}

	//------------------------------------------------------------------------------------------------
	static void Shutdown()
	{
		if (!s_Instance)
			return;

		s_Instance.ReleaseAll();
		s_Instance = null;
		ARIA_Log.Info("ARIA stopped");
	}

	//------------------------------------------------------------------------------------------------
	static ARIA_Manager GetInstance()
	{
		return s_Instance;
	}

	//------------------------------------------------------------------------------------------------
	void ARIA_Manager(ARIA_Settings settings)
	{
		m_Settings = settings;
		m_Comms = new ARIA_Comms(this);
		m_AirOps = new ARIA_AirOps(this);
	}

	//------------------------------------------------------------------------------------------------
	ARIA_Settings GetSettings()
	{
		return m_Settings;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_Comms GetComms()
	{
		return m_Comms;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_AirOps GetAirOps()
	{
		return m_AirOps;
	}

	//------------------------------------------------------------------------------------------------
	//! Seconds since ARIA started (monotonic, pauses with the game).
	float GetTime()
	{
		return m_fTime;
	}

	//------------------------------------------------------------------------------------------------
	array<ref ARIA_GroupBrain> GetGroupBrains()
	{
		return m_aGroups;
	}

	//------------------------------------------------------------------------------------------------
	array<ref ARIA_HeliPilot> GetHeliPilots()
	{
		return m_aHelis;
	}

	//------------------------------------------------------------------------------------------------
	//! Re-reads the settings file (admin command "#aria reload").
	void ReloadSettings()
	{
		ARIA_Settings settings = ARIA_SettingsIO.Load();
		ApplySettings(settings);
	}

	//------------------------------------------------------------------------------------------------
	void ApplySettings(ARIA_Settings settings)
	{
		m_Settings = settings;
		ARIA_Log.s_eLevel = settings.m_iLogLevel;
		OnSettingsChanged();
	}

	//------------------------------------------------------------------------------------------------
	//! Propagates runtime setting changes to live brains.
	void OnSettingsChanged()
	{
		foreach (ARIA_GroupBrain brain : m_aGroups)
		{
			brain.OnSettingsChanged();
		}

		foreach (ARIA_HeliPilot pilot : m_aHelis)
		{
			pilot.OnSettingsChanged();
		}
	}

	//------------------------------------------------------------------------------------------------
	void OnFrame(float dt)
	{
		if (dt <= 0)
			return;

		// Large hitches (loading, pause) would destabilise the flight controller
		dt = Math.Min(dt, 0.1);
		m_fTime += dt;

		m_fDiscoveryTimer -= dt;
		if (m_fDiscoveryTimer <= 0)
		{
			m_fDiscoveryTimer = DISCOVERY_INTERVAL;
			Discover();
			Cleanup();
		}

		m_Comms.Update(m_fTime);
		UpdateGroups();
		UpdateVehicles();
		UpdateHelis(dt);
		m_AirOps.Update(m_fTime);
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateGroups()
	{
		if (!m_Settings.m_Squad.m_bEnabled)
			return;

		int count = m_aGroups.Count();
		if (count == 0)
			return;

		int budget = Math.Min(m_Settings.m_iMaxGroupsPerFrame, count);
		for (int i = 0; i < budget; i++)
		{
			if (m_iGroupCursor >= count)
				m_iGroupCursor = 0;

			ARIA_GroupBrain brain = m_aGroups[m_iGroupCursor];
			m_iGroupCursor++;
			if (brain.IsValid() && brain.IsThinkDue(m_fTime))
				brain.Think(m_fTime);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateVehicles()
	{
		if (!m_Settings.m_Vehicles.m_bEnabled)
			return;

		int count = m_aVehicles.Count();
		if (count == 0)
			return;

		int budget = Math.Min(4, count);
		for (int i = 0; i < budget; i++)
		{
			if (m_iVehicleCursor >= count)
				m_iVehicleCursor = 0;

			ARIA_VehicleBrain brain = m_aVehicles[m_iVehicleCursor];
			m_iVehicleCursor++;
			if (brain.IsValid() && brain.IsThinkDue(m_fTime))
				brain.Think(m_fTime);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateHelis(float dt)
	{
		if (!m_Settings.m_Heli.m_bEnabled)
			return;

		foreach (ARIA_HeliPilot pilot : m_aHelis)
		{
			if (pilot.IsValid())
				pilot.Update(m_fTime, dt);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Finds new AI groups and the vehicles/helicopters their members crew.
	protected void Discover()
	{
		AIWorld aiWorld = GetGame().GetAIWorld();
		if (!aiWorld)
			return;

		array<AIAgent> agents = {};
		aiWorld.GetAIAgents(agents);
		foreach (AIAgent agent : agents)
		{
			SCR_AIGroup group = SCR_AIGroup.Cast(agent);
			if (!group || FindGroupBrain(group))
				continue;

			if (!ShouldManageGroup(group))
				continue;

			ARIA_GroupBrain brain = new ARIA_GroupBrain(this, group);
			m_aGroups.Insert(brain);
			ARIA_Log.Debug("Managing group " + brain.GetDebugName());
		}

		foreach (ARIA_GroupBrain brain : m_aGroups)
		{
			if (brain.IsValid())
				DiscoverCrewedVehicles(brain);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void DiscoverCrewedVehicles(ARIA_GroupBrain brain)
	{
		array<AIAgent> members = {};
		brain.GetGroup().GetAgents(members);
		foreach (AIAgent member : members)
		{
			IEntity character = member.GetControlledEntity();
			IEntity vehicle = ARIA_EntityUtils.GetVehicle(character);
			if (!vehicle)
				continue;

			if (ARIA_VehicleUtils.IsHelicopter(vehicle))
			{
				if (m_Settings.m_Heli.m_bEnabled && !FindHeliPilot(vehicle) && ARIA_VehicleUtils.IsPilotOf(character, vehicle))
				{
					ARIA_HeliPilot pilot = new ARIA_HeliPilot(this, vehicle, brain);
					if (pilot.IsValid())
					{
						m_aHelis.Insert(pilot);
						brain.SetHelicopter(pilot);
						ARIA_Log.Info("AI pilot takes control of " + ARIA_VehicleUtils.GetDebugName(vehicle) + " (" + brain.GetDebugName() + ")");
					}
				}
				continue;
			}

			if (m_Settings.m_Vehicles.m_bEnabled && !FindVehicleBrain(vehicle) && !ARIA_VehicleUtils.IsStaticWeapon(vehicle))
			{
				ARIA_VehicleBrain vehicleBrain = new ARIA_VehicleBrain(this, vehicle, brain);
				m_aVehicles.Insert(vehicleBrain);
				ARIA_Log.Debug("Managing vehicle " + ARIA_VehicleUtils.GetDebugName(vehicle) + " (" + brain.GetDebugName() + ")");
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	protected bool ShouldManageGroup(SCR_AIGroup group)
	{
		if (group.IsDormant())
			return false;

		if (group.GetPlayerCount() > 0 && !m_Settings.m_Squad.m_bAffectPlayerLedGroups)
			return false;

		if (m_Settings.IsFactionExcluded(group.GetFactionName()))
			return false;

		return group.GetAgentsCount() > 0;
	}

	//------------------------------------------------------------------------------------------------
	protected void Cleanup()
	{
		for (int i = m_aHelis.Count() - 1; i >= 0; i--)
		{
			if (!m_aHelis[i].IsValid())
			{
				m_aHelis[i].Release();
				m_aHelis.Remove(i);
			}
		}

		for (int i = m_aVehicles.Count() - 1; i >= 0; i--)
		{
			if (!m_aVehicles[i].IsValid())
				m_aVehicles.Remove(i);
		}

		for (int i = m_aGroups.Count() - 1; i >= 0; i--)
		{
			ARIA_GroupBrain brain = m_aGroups[i];
			if (brain.IsValid())
				continue;

			brain.Release();
			m_aGroups.Remove(i);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void ReleaseAll()
	{
		foreach (ARIA_HeliPilot pilot : m_aHelis)
		{
			pilot.Release();
		}

		foreach (ARIA_GroupBrain brain : m_aGroups)
		{
			brain.Release();
		}

		m_aHelis.Clear();
		m_aVehicles.Clear();
		m_aGroups.Clear();
	}

	//------------------------------------------------------------------------------------------------
	ARIA_GroupBrain FindGroupBrain(AIGroup group)
	{
		if (!group)
			return null;

		foreach (ARIA_GroupBrain brain : m_aGroups)
		{
			if (brain.GetGroup() == group)
				return brain;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_VehicleBrain FindVehicleBrain(IEntity vehicle)
	{
		foreach (ARIA_VehicleBrain brain : m_aVehicles)
		{
			if (brain.GetVehicle() == vehicle)
				return brain;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_HeliPilot FindHeliPilot(IEntity helicopter)
	{
		foreach (ARIA_HeliPilot pilot : m_aHelis)
		{
			if (pilot.GetHelicopter() == helicopter)
				return pilot;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	//! Friendly squads (excluding `exclude`) whose centre is within radius of pos.
	void FindFriendlyGroups(Faction faction, vector pos, float radius, ARIA_GroupBrain exclude, notnull array<ARIA_GroupBrain> outBrains)
	{
		float radiusSq = radius * radius;
		foreach (ARIA_GroupBrain brain : m_aGroups)
		{
			if (brain == exclude || !brain.IsValid())
				continue;

			if (!ARIA_EntityUtils.AreFriends(faction, brain.GetFaction()))
				continue;

			if (vector.DistanceSq(brain.GetPosition(), pos) <= radiusSq)
				outBrains.Insert(brain);
		}
	}
}
