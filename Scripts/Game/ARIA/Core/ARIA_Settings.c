// ARIA - all user-tunable settings.
//
// Every value here can be changed by a server admin in
//   $profile:ARIA/ARIA_Settings.cfg
// (the file is generated with defaults + documentation on first start), at
// runtime with the admin chat command "#aria set <key> <value>", or from a
// scenario script via ARIA_Api.GetSettings().

enum ARIA_EPreset
{
	CASUAL,		//!< forgiving AI: slower reactions, less accurate, rarely flanks
	REALISTIC,	//!< default: competent, cautious, cooperative AI
	ELITE,		//!< hardcore: fast, accurate, aggressive, coordinated
	CUSTOM		//!< no preset baseline, only file values
}

//------------------------------------------------------------------------------------------------
//! Skill/behaviour profile. One "default" profile plus optional per-faction overrides.
class ARIA_SkillProfile
{
	string m_sFactionKey = "default";
	float m_fSkill = 0.6;			//!< 0..1 -> mapped to vanilla EAISkill
	float m_fPerception = 1.0;		//!< multiplier on spotting ability
	float m_fReactionTime = 0.6;	//!< seconds before a squad reacts to new contact
	float m_fCourage = 0.6;			//!< 0..1, higher = withdraws later
	float m_fAggression = 0.5;		//!< 0..1, higher = assaults/flanks more
	float m_fTeamwork = 0.8;		//!< 0..1, chance to share intel / answer help calls

	//------------------------------------------------------------------------------------------------
	void Visit(ARIA_ConfigVisitor v)
	{
		v.BeginSection("skill." + m_sFactionKey, "Skill profile: " + m_sFactionKey);
		m_fSkill = v.Float("skill", m_fSkill, 0, 1, "General combat skill (0 recruit .. 1 elite)");
		m_fPerception = v.Float("perception", m_fPerception, 0.1, 3, "Spotting multiplier");
		m_fReactionTime = v.Float("reactionTime", m_fReactionTime, 0, 10, "Seconds before squad reacts to new contact");
		m_fCourage = v.Float("courage", m_fCourage, 0, 1, "Higher = holds ground longer before withdrawing");
		m_fAggression = v.Float("aggression", m_fAggression, 0, 1, "Higher = assaults and flanks more often");
		m_fTeamwork = v.Float("teamwork", m_fTeamwork, 0, 1, "Chance to share intel and answer calls for help");
	}

	//------------------------------------------------------------------------------------------------
	void CopyFrom(ARIA_SkillProfile other)
	{
		m_fSkill = other.m_fSkill;
		m_fPerception = other.m_fPerception;
		m_fReactionTime = other.m_fReactionTime;
		m_fCourage = other.m_fCourage;
		m_fAggression = other.m_fAggression;
		m_fTeamwork = other.m_fTeamwork;
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_SquadSettings
{
	bool m_bEnabled = true;
	bool m_bAffectPlayerLedGroups = false;
	float m_fThinkInterval = 1.0;

	// Knowledge
	float m_fContactMemory = 90;
	bool m_bShareIntel = true;
	float m_fIntelShareRadius = 1200;
	float m_fIntelRadioDelay = 4;

	// Tactics
	bool m_bFlanking = true;
	float m_fFlankMinDistance = 80;
	float m_fFlankMaxDistance = 600;
	bool m_bSuppressAndHold = true;
	bool m_bWithdraw = true;
	float m_fWithdrawCasualtyRatio = 0.5;
	float m_fWithdrawForceRatio = 2.5;
	float m_fWithdrawDistance = 250;
	float m_fEngageRange = 800;

	// Cooperation
	bool m_bReinforcements = true;
	float m_fReinforceRadius = 1000;
	int m_iMaxReinforcingGroups = 2;
	float m_fCombatTimeout = 60;

	// Assets
	bool m_bUseStaticWeapons = true;
	float m_fStaticWeaponRadius = 60;
	bool m_bUseNearbyVehicles = true;
	float m_fVehicleSearchRadius = 150;
	float m_fMinTravelForVehicle = 700;

	//------------------------------------------------------------------------------------------------
	void Visit(ARIA_ConfigVisitor v)
	{
		v.BeginSection("squad", "Squad / infantry group AI");
		m_bEnabled = v.Bool("enabled", m_bEnabled, "Enable ARIA squad tactics");
		m_bAffectPlayerLedGroups = v.Bool("affectPlayerLedGroups", m_bAffectPlayerLedGroups, "Also manage AI groups led by a player");
		m_fThinkInterval = v.Float("thinkInterval", m_fThinkInterval, 0.25, 10, "Seconds between squad decisions");

		v.Comment("Knowledge and communication");
		m_fContactMemory = v.Float("contactMemory", m_fContactMemory, 10, 600, "Seconds an unseen enemy stays in squad memory");
		m_bShareIntel = v.Bool("shareIntel", m_bShareIntel, "Squads radio enemy positions to friendly squads");
		m_fIntelShareRadius = v.Float("intelShareRadius", m_fIntelShareRadius, 50, 10000, "Radio range in meters");
		m_fIntelRadioDelay = v.Float("intelRadioDelay", m_fIntelRadioDelay, 0, 60, "Seconds of radio delay before intel arrives");

		v.Comment("Tactics");
		m_bFlanking = v.Bool("flanking", m_bFlanking, "Squads try to flank known enemy positions");
		m_fFlankMinDistance = v.Float("flankMinDistance", m_fFlankMinDistance, 20, 1000, "Do not flank enemies closer than this");
		m_fFlankMaxDistance = v.Float("flankMaxDistance", m_fFlankMaxDistance, 50, 3000, "Do not flank enemies farther than this");
		m_bSuppressAndHold = v.Bool("suppressAndHold", m_bSuppressAndHold, "Outgunned squads hold position and suppress");
		m_bWithdraw = v.Bool("withdraw", m_bWithdraw, "Broken squads withdraw instead of dying in place");
		m_fWithdrawCasualtyRatio = v.Float("withdrawCasualtyRatio", m_fWithdrawCasualtyRatio, 0.1, 1, "Losses (fraction) that can trigger withdrawal");
		m_fWithdrawForceRatio = v.Float("withdrawForceRatio", m_fWithdrawForceRatio, 1, 10, "Enemy/friendly strength ratio that can trigger withdrawal");
		m_fWithdrawDistance = v.Float("withdrawDistance", m_fWithdrawDistance, 50, 2000, "How far squads fall back");
		m_fEngageRange = v.Float("engageRange", m_fEngageRange, 100, 3000, "Max distance at which a squad treats a contact as a fight");

		v.Comment("Cooperation");
		m_bReinforcements = v.Bool("reinforcements", m_bReinforcements, "Idle squads move to help squads in contact");
		m_fReinforceRadius = v.Float("reinforceRadius", m_fReinforceRadius, 100, 5000, "Max distance for a squad to answer a call for help");
		m_iMaxReinforcingGroups = v.Int("maxReinforcingGroups", m_iMaxReinforcingGroups, 0, 10, "Squads that may answer one call");
		m_fCombatTimeout = v.Float("combatTimeout", m_fCombatTimeout, 10, 600, "Seconds without contact before combat is considered over");

		v.Comment("Assets");
		m_bUseStaticWeapons = v.Bool("useStaticWeapons", m_bUseStaticWeapons, "Squads in a fight man free static weapons (MGs, launchers) nearby");
		m_fStaticWeaponRadius = v.Float("staticWeaponRadius", m_fStaticWeaponRadius, 10, 300, "Search radius for static weapons");
		m_bUseNearbyVehicles = v.Bool("useNearbyVehicles", m_bUseNearbyVehicles, "Squads take free friendly vehicles for long moves");
		m_fVehicleSearchRadius = v.Float("vehicleSearchRadius", m_fVehicleSearchRadius, 20, 1000, "Search radius for free vehicles");
		m_fMinTravelForVehicle = v.Float("minTravelForVehicle", m_fMinTravelForVehicle, 100, 10000, "Only mount up for moves longer than this");
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_VehicleSettings
{
	bool m_bEnabled = true;
	float m_fThinkInterval = 0.5;

	bool m_bFillTurrets = true;
	bool m_bDismountOnContact = true;
	float m_fDismountDistance = 400;
	bool m_bCrewSupportsDismounts = true;

	bool m_bStuckRecovery = true;
	float m_fStuckTime = 8;
	float m_fStuckSpeed = 0.7;
	bool m_bFlipRecovery = true;
	float m_fFlipTime = 6;

	bool m_bBailOutWhenCrippled = true;
	float m_fBailOutHealth = 0.25;

	float m_fConvoyMaxSpeedKmh = 60;

	//------------------------------------------------------------------------------------------------
	void Visit(ARIA_ConfigVisitor v)
	{
		v.BeginSection("vehicle", "Ground vehicles (cars, trucks, APCs, IFVs, static weapons)");
		m_bEnabled = v.Bool("enabled", m_bEnabled, "Enable ARIA ground vehicle handling");
		m_fThinkInterval = v.Float("thinkInterval", m_fThinkInterval, 0.1, 5, "Seconds between vehicle decisions");

		v.Comment("Crew");
		m_bFillTurrets = v.Bool("fillTurrets", m_bFillTurrets, "Passengers move into empty turret seats in combat");
		m_bDismountOnContact = v.Bool("dismountOnContact", m_bDismountOnContact, "Infantry in transports dismount when contact is near");
		m_fDismountDistance = v.Float("dismountDistance", m_fDismountDistance, 50, 2000, "Contact distance that triggers a dismount");
		m_bCrewSupportsDismounts = v.Bool("crewSupportsDismounts", m_bCrewSupportsDismounts, "Driver+gunner of armed vehicles stay and give fire support");

		v.Comment("Driving recovery");
		m_bStuckRecovery = v.Bool("stuckRecovery", m_bStuckRecovery, "Detect and recover stuck vehicles");
		m_fStuckTime = v.Float("stuckTime", m_fStuckTime, 2, 60, "Seconds without progress before a vehicle is stuck");
		m_fStuckSpeed = v.Float("stuckSpeed", m_fStuckSpeed, 0.1, 5, "Speed (m/s) under which a driving vehicle counts as not moving");
		m_bFlipRecovery = v.Bool("flipRecovery", m_bFlipRecovery, "Put flipped vehicles back on their wheels");
		m_fFlipTime = v.Float("flipTime", m_fFlipTime, 1, 60, "Seconds upside down before flip recovery");

		v.Comment("Damage");
		m_bBailOutWhenCrippled = v.Bool("bailOutWhenCrippled", m_bBailOutWhenCrippled, "Crew bails out of crippled vehicles");
		m_fBailOutHealth = v.Float("bailOutHealth", m_fBailOutHealth, 0, 1, "Hull health fraction that counts as crippled");

		v.Comment("Convoys");
		m_fConvoyMaxSpeedKmh = v.Float("convoyMaxSpeedKmh", m_fConvoyMaxSpeedKmh, 10, 150, "Speed cap for multi-vehicle groups");
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_HeliSettings
{
	bool m_bEnabled = true;

	// Flight envelope
	float m_fCruiseSpeedKmh = 170;
	float m_fCombatSpeedKmh = 110;
	float m_fCruiseAltitude = 120;
	float m_fCombatAltitude = 60;
	float m_fNapOfEarthAltitude = 25;
	float m_fMinAltitude = 15;
	float m_fMaxTiltDeg = 28;
	float m_fMaxClimbRate = 9;
	float m_fMaxDescentRate = 7;
	float m_fLandingDescentRate = 2;
	float m_fMaxYawRateDeg = 45;
	float m_fMaxAccel = 4.5;
	float m_fTerrainLookahead = 6;
	float m_fObstacleClearance = 25;

	// Landing zones
	float m_fLZSearchRadius = 150;
	float m_fLZMaxSlopeDeg = 12;
	float m_fLZClearRadius = 12;
	float m_fUnloadTimeout = 45;
	float m_fBoardTimeout = 120;

	// Combat
	float m_fCASOrbitRadius = 450;
	float m_fCASOrbitAltitude = 140;
	float m_fCASOrbitSpeedKmh = 90;
	float m_fCASStationTime = 240;
	bool m_bEvadeWhenHit = true;
	bool m_bRTBWhenDamaged = true;
	float m_fRTBHealth = 0.45;
	bool m_bCopilotTakeover = true;

	// Automatic support missions
	bool m_bAutoAirlift = true;
	float m_fAirliftMinDistance = 2000;
	float m_fAirliftSearchRadius = 4000;
	float m_fAirliftLZOffset = 350;
	bool m_bAutoCAS = true;
	float m_fCASRequestRadius = 5000;

	// Flight controller tuning (advanced)
	int m_iFlightModel = ARIA_EFlightModel.VELOCITY;
	float m_fVelocityGain = 0.9;
	float m_fVelocityIntegral = 0.25;
	float m_fAltitudeGain = 0.6;
	float m_fAttitudeGain = 3.5;
	float m_fYawGain = 1.6;
	float m_fAngularResponse = 6;
	float m_fHorizontalAssist = 0.25;
	float m_fMaxThrustG = 2.0;
	float m_fSpoolUpTime = 12;

	//------------------------------------------------------------------------------------------------
	float GetCruiseSpeed()
	{
		return m_fCruiseSpeedKmh / 3.6;
	}

	//------------------------------------------------------------------------------------------------
	float GetCombatSpeed()
	{
		return m_fCombatSpeedKmh / 3.6;
	}

	//------------------------------------------------------------------------------------------------
	float GetOrbitSpeed()
	{
		return m_fCASOrbitSpeedKmh / 3.6;
	}

	//------------------------------------------------------------------------------------------------
	void Visit(ARIA_ConfigVisitor v)
	{
		v.BeginSection("heli", "Helicopters (AI pilots - new feature)");
		m_bEnabled = v.Bool("enabled", m_bEnabled, "AI pilots fly helicopters");

		v.Comment("Flight envelope");
		m_fCruiseSpeedKmh = v.Float("cruiseSpeedKmh", m_fCruiseSpeedKmh, 40, 320, "Transit speed");
		m_fCombatSpeedKmh = v.Float("combatSpeedKmh", m_fCombatSpeedKmh, 30, 320, "Speed near the enemy");
		m_fCruiseAltitude = v.Float("cruiseAltitude", m_fCruiseAltitude, 20, 1500, "Transit altitude above ground (m)");
		m_fCombatAltitude = v.Float("combatAltitude", m_fCombatAltitude, 15, 1000, "Altitude above ground near the enemy (m)");
		m_fNapOfEarthAltitude = v.Float("napOfEarthAltitude", m_fNapOfEarthAltitude, 8, 200, "Low-level altitude used when evading (m)");
		m_fMinAltitude = v.Float("minAltitude", m_fMinAltitude, 5, 200, "Never fly lower than this above obstacles, except landing (m)");
		m_fMaxTiltDeg = v.Float("maxTiltDeg", m_fMaxTiltDeg, 5, 60, "Max pitch/bank angle");
		m_fMaxClimbRate = v.Float("maxClimbRate", m_fMaxClimbRate, 1, 30, "Max climb rate (m/s)");
		m_fMaxDescentRate = v.Float("maxDescentRate", m_fMaxDescentRate, 1, 30, "Max descent rate (m/s)");
		m_fLandingDescentRate = v.Float("landingDescentRate", m_fLandingDescentRate, 0.3, 6, "Final touchdown descent rate (m/s)");
		m_fMaxYawRateDeg = v.Float("maxYawRateDeg", m_fMaxYawRateDeg, 5, 180, "Max turn rate (deg/s)");
		m_fMaxAccel = v.Float("maxAccel", m_fMaxAccel, 1, 15, "Max horizontal acceleration (m/s2)");
		m_fTerrainLookahead = v.Float("terrainLookahead", m_fTerrainLookahead, 1, 20, "Seconds of terrain look-ahead for terrain following");
		m_fObstacleClearance = v.Float("obstacleClearance", m_fObstacleClearance, 5, 100, "Clearance kept above trees/buildings (m)");

		v.Comment("Landing zones");
		m_fLZSearchRadius = v.Float("lzSearchRadius", m_fLZSearchRadius, 20, 1000, "Search radius for a safe landing spot");
		m_fLZMaxSlopeDeg = v.Float("lzMaxSlopeDeg", m_fLZMaxSlopeDeg, 2, 30, "Max ground slope for landing");
		m_fLZClearRadius = v.Float("lzClearRadius", m_fLZClearRadius, 5, 40, "Obstacle-free radius required for landing (rotor clearance)");
		m_fUnloadTimeout = v.Float("unloadTimeout", m_fUnloadTimeout, 5, 300, "Max seconds on the ground while unloading");
		m_fBoardTimeout = v.Float("boardTimeout", m_fBoardTimeout, 10, 600, "Max seconds waiting for passengers at pickup");

		v.Comment("Combat");
		m_fCASOrbitRadius = v.Float("casOrbitRadius", m_fCASOrbitRadius, 100, 3000, "Orbit radius around the target for door gunners");
		m_fCASOrbitAltitude = v.Float("casOrbitAltitude", m_fCASOrbitAltitude, 30, 1500, "Orbit altitude above ground");
		m_fCASOrbitSpeedKmh = v.Float("casOrbitSpeedKmh", m_fCASOrbitSpeedKmh, 30, 250, "Orbit speed");
		m_fCASStationTime = v.Float("casStationTime", m_fCASStationTime, 30, 1800, "Seconds on station before returning");
		m_bEvadeWhenHit = v.Bool("evadeWhenHit", m_bEvadeWhenHit, "Break off low and fast when taking hits");
		m_bRTBWhenDamaged = v.Bool("rtbWhenDamaged", m_bRTBWhenDamaged, "Return to base when badly damaged");
		m_fRTBHealth = v.Float("rtbHealth", m_fRTBHealth, 0, 1, "Hull health fraction that triggers RTB");
		m_bCopilotTakeover = v.Bool("copilotTakeover", m_bCopilotTakeover, "Co-pilot takes the controls if the pilot dies");

		v.Comment("Automatic support (air mobility)");
		m_bAutoAirlift = v.Bool("autoAirlift", m_bAutoAirlift, "Idle transport helicopters fly infantry squads to far waypoints");
		m_fAirliftMinDistance = v.Float("airliftMinDistance", m_fAirliftMinDistance, 300, 20000, "Min move distance to request an airlift");
		m_fAirliftSearchRadius = v.Float("airliftSearchRadius", m_fAirliftSearchRadius, 500, 30000, "Max distance from the helicopter to the squad");
		m_fAirliftLZOffset = v.Float("airliftLZOffset", m_fAirliftLZOffset, 0, 2000, "Land this far short of the objective");
		m_bAutoCAS = v.Bool("autoCAS", m_bAutoCAS, "Armed helicopters answer calls for close air support");
		m_fCASRequestRadius = v.Float("casRequestRadius", m_fCASRequestRadius, 500, 30000, "Max distance for CAS requests");

		v.Comment("Flight controller tuning (advanced - change carefully)");
		m_iFlightModel = v.Int("ctl.flightModel", m_iFlightModel, 0, 1, "0 = velocity model (robust, default), 1 = force model (experimental, pure physics)");
		m_fVelocityGain = v.Float("ctl.velocityGain", m_fVelocityGain, 0.05, 5, "Velocity tracking gain");
		m_fVelocityIntegral = v.Float("ctl.velocityIntegral", m_fVelocityIntegral, 0, 3, "Velocity integral gain (wind / lift compensation)");
		m_fAltitudeGain = v.Float("ctl.altitudeGain", m_fAltitudeGain, 0.05, 5, "Altitude error -> climb rate gain");
		m_fAttitudeGain = v.Float("ctl.attitudeGain", m_fAttitudeGain, 0.2, 20, "Attitude error -> angular rate gain");
		m_fYawGain = v.Float("ctl.yawGain", m_fYawGain, 0.1, 10, "Heading error -> yaw rate gain");
		m_fAngularResponse = v.Float("ctl.angularResponse", m_fAngularResponse, 0.5, 30, "How fast the airframe follows commanded rates");
		m_fHorizontalAssist = v.Float("ctl.horizontalAssist", m_fHorizontalAssist, 0, 1, "Direct horizontal force assist (0 = pure tilt physics)");
		m_fMaxThrustG = v.Float("ctl.maxThrustG", m_fMaxThrustG, 1.1, 4, "Max rotor thrust in g");
		m_fSpoolUpTime = v.Float("ctl.spoolUpTime", m_fSpoolUpTime, 0, 60, "Max seconds to wait for rotor spool-up before takeoff");
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_Settings
{
	static const int VERSION = 1;

	bool m_bEnabled = true;
	int m_iLogLevel = ARIA_ELogLevel.INFO;
	string m_sPreset = "REALISTIC";
	string m_sExcludedFactions = "";
	int m_iMaxGroupsPerFrame = 6;

	ref ARIA_SquadSettings m_Squad = new ARIA_SquadSettings();
	ref ARIA_VehicleSettings m_Vehicles = new ARIA_VehicleSettings();
	ref ARIA_HeliSettings m_Heli = new ARIA_HeliSettings();
	ref ARIA_SkillProfile m_DefaultSkill = new ARIA_SkillProfile();
	ref array<ref ARIA_SkillProfile> m_aFactionSkills = {};

	protected ref array<string> m_aExcludedFactionList = {};

	//------------------------------------------------------------------------------------------------
	void Visit(ARIA_ConfigVisitor v)
	{
		v.BeginSection("general", "ARIA - Advanced Reforger Intelligent AI  (settings version " + VERSION + ")");
		v.Comment("Edit values and restart the server, or use '#aria reload' / '#aria set <key> <value>' as admin.");
		m_bEnabled = v.Bool("enabled", m_bEnabled, "Master switch for the whole mod");
		m_sPreset = v.Str("preset", m_sPreset, "Baseline applied before this file: CASUAL, REALISTIC, ELITE or CUSTOM");
		m_iLogLevel = v.Int("logLevel", m_iLogLevel, 0, 4, "0=errors 1=warnings 2=info 3=debug 4=verbose");
		m_sExcludedFactions = v.Str("excludedFactions", m_sExcludedFactions, "Comma separated faction keys ARIA must not touch, e.g. CIV,FIA");
		m_iMaxGroupsPerFrame = v.Int("maxGroupsPerFrame", m_iMaxGroupsPerFrame, 1, 100, "Performance: squads evaluated per frame");

		v.Comment("");
		v.Comment("Everything below is commented out so the preset decides the value.");
		v.Comment("Remove the leading '#' of a line to override that single value.");
		v.SetCommentValues(true);

		m_Squad.Visit(v);
		m_Vehicles.Visit(v);
		m_Heli.Visit(v);

		m_DefaultSkill.m_sFactionKey = "default";
		m_DefaultSkill.Visit(v);

		VisitFactionSkills(v);
		v.SetCommentValues(false);
		RebuildExcludedFactions();
	}

	//------------------------------------------------------------------------------------------------
	//! Per-faction profiles are discovered from keys like "skill.USSR.skill".
	protected void VisitFactionSkills(ARIA_ConfigVisitor v)
	{
		if (v.GetMode() == ARIA_EConfigMode.LOAD)
		{
			array<string> keys = {};
			v.GetKeysWithPrefix("skill.", keys);
			foreach (string key : keys)
			{
				// skill.<FACTION>.<field>
				array<string> parts = {};
				key.Split(".", parts, true);
				if (parts.Count() < 3)
					continue;

				string factionKey = parts[1];
				if (factionKey == "default")
					continue;

				if (!FindFactionSkill(factionKey))
				{
					ARIA_SkillProfile profile = new ARIA_SkillProfile();
					profile.m_sFactionKey = factionKey;
					profile.CopyFrom(m_DefaultSkill);
					m_aFactionSkills.Insert(profile);
				}
			}
		}

		if (v.GetMode() == ARIA_EConfigMode.SAVE && m_aFactionSkills.IsEmpty())
		{
			v.Comment("");
			v.Comment("Per-faction overrides: copy the 'skill.default' block and replace 'default' with a faction key,");
			v.Comment("for example 'skill.USSR.skill = 0.8'. Unlisted values inherit from skill.default.");
		}

		foreach (ARIA_SkillProfile profile : m_aFactionSkills)
		{
			profile.Visit(v);
		}
	}

	//------------------------------------------------------------------------------------------------
	ARIA_SkillProfile FindFactionSkill(string factionKey)
	{
		foreach (ARIA_SkillProfile profile : m_aFactionSkills)
		{
			if (profile.m_sFactionKey == factionKey)
				return profile;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_SkillProfile GetSkillProfile(string factionKey)
	{
		ARIA_SkillProfile profile = FindFactionSkill(factionKey);
		if (profile)
			return profile;

		return m_DefaultSkill;
	}

	//------------------------------------------------------------------------------------------------
	bool IsFactionExcluded(string factionKey)
	{
		if (factionKey.IsEmpty())
			return false;

		return m_aExcludedFactionList.Contains(factionKey);
	}

	//------------------------------------------------------------------------------------------------
	protected void RebuildExcludedFactions()
	{
		m_aExcludedFactionList.Clear();
		array<string> parts = {};
		m_sExcludedFactions.Split(",", parts, true);
		foreach (string part : parts)
		{
			string trimmed = part.Trim();
			if (!trimmed.IsEmpty())
				m_aExcludedFactionList.Insert(trimmed);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Baseline values for a preset. Called before the file is applied.
	void ApplyPreset(string presetName)
	{
		string preset = presetName;
		preset.ToUpper();

		if (preset == "CASUAL")
		{
			m_DefaultSkill.m_fSkill = 0.35;
			m_DefaultSkill.m_fPerception = 0.7;
			m_DefaultSkill.m_fReactionTime = 2.0;
			m_DefaultSkill.m_fCourage = 0.4;
			m_DefaultSkill.m_fAggression = 0.3;
			m_DefaultSkill.m_fTeamwork = 0.5;
			m_Squad.m_fIntelRadioDelay = 10;
			m_Squad.m_iMaxReinforcingGroups = 1;
			return;
		}

		if (preset == "ELITE")
		{
			m_DefaultSkill.m_fSkill = 0.9;
			m_DefaultSkill.m_fPerception = 1.35;
			m_DefaultSkill.m_fReactionTime = 0.25;
			m_DefaultSkill.m_fCourage = 0.8;
			m_DefaultSkill.m_fAggression = 0.75;
			m_DefaultSkill.m_fTeamwork = 1.0;
			m_Squad.m_fIntelRadioDelay = 2;
			m_Squad.m_iMaxReinforcingGroups = 3;
			m_Heli.m_fMaxTiltDeg = 32;
			return;
		}

		if (preset == "REALISTIC")
		{
			// Class defaults are the REALISTIC preset.
			return;
		}
	}
}
