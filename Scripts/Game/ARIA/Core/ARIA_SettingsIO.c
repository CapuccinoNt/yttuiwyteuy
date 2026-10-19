// ARIA - loads/saves ARIA_Settings from the server profile folder.
//
//   Dedicated server: <profile>/ARIA/ARIA_Settings.cfg
//   Workbench / local: <user profile>/ARIA/ARIA_Settings.cfg
//
// If the file does not exist a fully documented default file is written.

class ARIA_SettingsIO
{
	static const string DIRECTORY = "$profile:ARIA";
	static const string FILE_PATH = "$profile:ARIA/ARIA_Settings.cfg";

	//------------------------------------------------------------------------------------------------
	//! Loads the settings; falls back to defaults if the file cannot be read.
	static ARIA_Settings Load()
	{
		ARIA_Settings settings = TryLoad();
		if (settings)
			return settings;

		settings = new ARIA_Settings();
		settings.ApplyPreset(settings.m_sPreset);
		return settings;
	}

	//------------------------------------------------------------------------------------------------
	//! Returns null when the file exists but cannot be opened (keeps live settings on reload).
	static ARIA_Settings TryLoad()
	{
		ARIA_Settings settings = new ARIA_Settings();

		if (!FileIO.FileExists(FILE_PATH))
		{
			settings.ApplyPreset(settings.m_sPreset);
			Save(settings, true);
			ARIA_Log.Info("No settings file found, wrote defaults to " + FILE_PATH);
			return settings;
		}

		ARIA_ConfigVisitor visitor = new ARIA_ConfigVisitor(ARIA_EConfigMode.LOAD);
		FileHandle file = FileIO.OpenFile(FILE_PATH, FileMode.READ);
		if (!file)
		{
			ARIA_Log.Warning("Could not open " + FILE_PATH);
			return null;
		}

		string line;
		while (file.ReadLine(line) > -1)
		{
			visitor.ParseLine(line);
		}

		file.Close();

		// The preset is a baseline: apply it first, then every explicit value from the file.
		string preset = settings.m_sPreset;
		if (visitor.HasKey("general.preset"))
			preset = visitor.GetRawValue("general.preset");

		settings.ApplyPreset(preset);
		settings.Visit(visitor);

		array<string> unused = {};
		visitor.GetUnusedKeys(unused);
		foreach (string key : unused)
		{
			ARIA_Log.Warning("Unknown setting '" + key + "' in " + FILE_PATH + " (typo?)");
		}

		ARIA_Log.Info("Settings loaded from " + FILE_PATH + " (preset " + preset + ")");
		return settings;
	}

	//------------------------------------------------------------------------------------------------
	//! \param asDefaults true = first-start file: preset-driven values are written commented out
	static bool Save(ARIA_Settings settings, bool asDefaults = false)
	{
		FileIO.MakeDirectory(DIRECTORY);

		ARIA_ConfigVisitor visitor = new ARIA_ConfigVisitor(ARIA_EConfigMode.SAVE);
		visitor.AllowCommentedValues(asDefaults);
		settings.Visit(visitor);

		FileHandle file = FileIO.OpenFile(FILE_PATH, FileMode.WRITE);
		if (!file)
		{
			ARIA_Log.Warning("Could not write " + FILE_PATH);
			return false;
		}

		foreach (string line : visitor.GetLines())
		{
			file.WriteLine(line);
		}

		file.Close();
		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Current value of one setting as written in the file (for admin replies).
	static string GetValueText(ARIA_Settings settings, string key)
	{
		ARIA_ConfigVisitor visitor = new ARIA_ConfigVisitor(ARIA_EConfigMode.SAVE);
		settings.Visit(visitor);
		string prefix = key + " = ";
		foreach (string line : visitor.GetLines())
		{
			if (!line.StartsWith(prefix))
				continue;

			string rest = line.Substring(prefix.Length(), line.Length() - prefix.Length());
			int hash = rest.IndexOf("#");
			if (hash >= 0)
				rest = rest.Substring(0, hash);

			return rest.Trim();
		}

		return "?";
	}

	//------------------------------------------------------------------------------------------------
	//! Applies a single "key value" change at runtime. Returns false for unknown keys.
	static bool ApplyOne(ARIA_Settings settings, string key, string value)
	{
		ARIA_ConfigVisitor visitor = new ARIA_ConfigVisitor(ARIA_EConfigMode.SET_ONE);
		visitor.PrepareSetOne(key, value);
		settings.Visit(visitor);
		return visitor.WasSetApplied();
	}
}
