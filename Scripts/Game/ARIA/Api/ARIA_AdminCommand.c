// ARIA - admin chat / RCON command.
//
//   #aria                      help
//   #aria status               managed squads, vehicles and helicopters
//   #aria reload               re-read $profile:ARIA/ARIA_Settings.cfg
//   #aria set <key> <value>    change one setting live, e.g. #aria set heli.cruiseSpeedKmh 200
//   #aria save                 write the current settings to the profile file

class ARIA_AdminCommand : ScrServerCommand
{
	//------------------------------------------------------------------------------------------------
	override string GetKeyword()
	{
		return "aria";
	}

	//------------------------------------------------------------------------------------------------
	override bool IsServerSide()
	{
		return true;
	}

	//------------------------------------------------------------------------------------------------
	override int RequiredRCONPermission()
	{
		return ERCONPermissions.PERMISSIONS_ADMIN;
	}

	//------------------------------------------------------------------------------------------------
	override int RequiredChatPermission()
	{
		return EPlayerRole.ADMINISTRATOR;
	}

	//------------------------------------------------------------------------------------------------
	override ref ScrServerCmdResult OnChatServerExecution(array<string> argv, int playerId)
	{
		return Execute(argv);
	}

	//------------------------------------------------------------------------------------------------
	override ref ScrServerCmdResult OnChatClientExecution(array<string> argv, int playerId)
	{
		return ScrServerCmdResult(string.Empty, EServerCmdResultType.OK);
	}

	//------------------------------------------------------------------------------------------------
	override ref ScrServerCmdResult OnRCONExecution(array<string> argv)
	{
		return Execute(argv);
	}

	//------------------------------------------------------------------------------------------------
	override ref ScrServerCmdResult OnUpdate()
	{
		return ScrServerCmdResult(string.Empty, EServerCmdResultType.OK);
	}

	//------------------------------------------------------------------------------------------------
	protected ScrServerCmdResult Execute(array<string> argv)
	{
		// argv[0] is the command keyword itself
		string sub;
		if (argv.Count() > 1)
			sub = argv[1];

		sub.ToLower();

		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (!manager)
		{
			// Disabled at boot: '#aria reload' after enabling it in the file starts ARIA
			if (sub == "reload")
			{
				ARIA_Manager.Start();
				if (ARIA_Manager.GetInstance())
					return ScrServerCmdResult("ARIA started", EServerCmdResultType.OK);
			}

			return ScrServerCmdResult("ARIA is not running (general.enabled = false in the settings file, or not the server)", EServerCmdResultType.ERR);
		}

		if (sub == "status")
			return ScrServerCmdResult(BuildStatus(manager), EServerCmdResultType.OK);

		if (sub == "reload")
		{
			if (!manager.ReloadSettings())
				return ScrServerCmdResult("Could not read " + ARIA_SettingsIO.FILE_PATH + ", live settings kept", EServerCmdResultType.ERR);

			return ScrServerCmdResult("ARIA settings reloaded", EServerCmdResultType.OK);
		}

		if (sub == "save")
		{
			if (ARIA_SettingsIO.Save(manager.GetSettings()))
				return ScrServerCmdResult("ARIA settings saved to " + ARIA_SettingsIO.FILE_PATH, EServerCmdResultType.OK);

			return ScrServerCmdResult("Could not write " + ARIA_SettingsIO.FILE_PATH, EServerCmdResultType.ERR);
		}

		if (sub == "set")
		{
			if (argv.Count() < 4)
				return ScrServerCmdResult("Usage: #aria set <key> <value>", EServerCmdResultType.PARAMETERS);

			// A preset is a baseline for the whole file, it cannot be swapped value by value
			if (argv[2] == "general.preset")
				return ScrServerCmdResult("Change general.preset in " + ARIA_SettingsIO.FILE_PATH + " and run '#aria reload'", EServerCmdResultType.ERR);

			if (!ARIA_SettingsIO.ApplyOne(manager.GetSettings(), argv[2], argv[3]))
				return ScrServerCmdResult("Unknown setting or invalid value: " + argv[2] + " " + argv[3], EServerCmdResultType.ERR);

			manager.OnSettingsChanged();
			return ScrServerCmdResult(argv[2] + " = " + ARIA_SettingsIO.GetValueText(manager.GetSettings(), argv[2]), EServerCmdResultType.OK);
		}

		return ScrServerCmdResult("#aria status | reload | save | set <key> <value>", EServerCmdResultType.OK);
	}

	//------------------------------------------------------------------------------------------------
	protected string BuildStatus(ARIA_Manager manager)
	{
		int squads = 0;
		int inCombat = 0;
		foreach (ARIA_GroupBrain brain : manager.GetGroupBrains())
		{
			if (!brain.IsValid())
				continue;

			squads++;
			if (brain.GetState() != ARIA_ESquadState.IDLE)
				inCombat++;
		}

		int helis = 0;
		int flying = 0;
		foreach (ARIA_HeliPilot pilot : manager.GetHeliPilots())
		{
			if (!pilot.IsValid())
				continue;

			helis++;
			ARIA_EHeliState state = pilot.GetState();
			if (state == ARIA_EHeliState.FLYING || state == ARIA_EHeliState.TAKEOFF || state == ARIA_EHeliState.LANDING)
				flying++;
		}

		return string.Format("ARIA: %1 squads (%2 engaged), %3 helicopters (%4 airborne), preset %5", squads, inCombat, helis, flying, manager.GetSettings().m_sPreset);
	}
}
