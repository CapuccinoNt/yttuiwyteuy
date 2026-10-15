// ARIA - tiny "key = value" configuration system.
//
// Every settings class implements Visit(ARIA_ConfigVisitor). The same Visit()
// method is used to LOAD values from the server profile file, to SAVE a fully
// documented default file, and to APPLY single values at runtime (admin
// commands / API). This keeps every setting declared in exactly one place.
//
// File format (one setting per line, '#' starts a comment):
//
//   [heli]
//   heli.cruiseSpeedKmh = 160      # Cruise speed in km/h (40..300)
//
// Section headers are cosmetic; keys are always fully qualified.

enum ARIA_EConfigMode
{
	LOAD,		//!< read values from m_mValues, keep current value when missing
	SAVE,		//!< write every value + its description to m_aLines
	SET_ONE		//!< apply only m_sSetKey/m_sSetValue
}

//------------------------------------------------------------------------------------------------
class ARIA_ConfigVisitor
{
	protected ARIA_EConfigMode m_eMode;
	protected ref map<string, string> m_mValues = new map<string, string>();
	protected ref set<string> m_UsedKeys = new set<string>();
	protected ref array<string> m_aLines = {};
	protected string m_sPrefix;
	protected bool m_bCommentValues;

	protected string m_sSetKey;
	protected string m_sSetValue;
	protected bool m_bSetApplied;

	//------------------------------------------------------------------------------------------------
	void ARIA_ConfigVisitor(ARIA_EConfigMode mode)
	{
		m_eMode = mode;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_EConfigMode GetMode()
	{
		return m_eMode;
	}

	//------------------------------------------------------------------------------------------------
	void SetRawValue(string key, string value)
	{
		m_mValues.Set(key, value);
	}

	//------------------------------------------------------------------------------------------------
	bool HasKey(string key)
	{
		return m_mValues.Contains(key);
	}

	//------------------------------------------------------------------------------------------------
	string GetRawValue(string key)
	{
		string value;
		m_mValues.Find(key, value);
		return value;
	}

	//------------------------------------------------------------------------------------------------
	//! All loaded keys starting with prefix (used to discover per-faction sections).
	void GetKeysWithPrefix(string prefix, notnull array<string> outKeys)
	{
		foreach (string key, string value : m_mValues)
		{
			if (key.StartsWith(prefix))
				outKeys.Insert(key);
		}
	}

	//------------------------------------------------------------------------------------------------
	void PrepareSetOne(string key, string value)
	{
		m_sSetKey = key;
		m_sSetValue = value;
		m_bSetApplied = false;
	}

	//------------------------------------------------------------------------------------------------
	bool WasSetApplied()
	{
		return m_bSetApplied;
	}

	//------------------------------------------------------------------------------------------------
	array<string> GetLines()
	{
		return m_aLines;
	}

	//------------------------------------------------------------------------------------------------
	//! Keys present in the file that no settings class consumed (typos).
	void GetUnusedKeys(notnull array<string> outKeys)
	{
		foreach (string key, string value : m_mValues)
		{
			if (!m_UsedKeys.Contains(key))
				outKeys.Insert(key);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! In SAVE mode, write values as commented-out lines ("#key = value") so the
	//! preset stays in charge until the admin uncomments a line to override it.
	void SetCommentValues(bool comment)
	{
		m_bCommentValues = comment;
	}

	//------------------------------------------------------------------------------------------------
	void BeginSection(string prefix, string title)
	{
		m_sPrefix = prefix;
		if (m_eMode == ARIA_EConfigMode.SAVE)
		{
			m_aLines.Insert("");
			m_aLines.Insert("#------------------------------------------------------------------------------");
			m_aLines.Insert("# " + title);
			m_aLines.Insert("#------------------------------------------------------------------------------");
		}
	}

	//------------------------------------------------------------------------------------------------
	void Comment(string text)
	{
		if (m_eMode == ARIA_EConfigMode.SAVE)
			m_aLines.Insert("# " + text);
	}

	//------------------------------------------------------------------------------------------------
	protected string FullKey(string key)
	{
		if (m_sPrefix.IsEmpty())
			return key;

		return m_sPrefix + "." + key;
	}

	//------------------------------------------------------------------------------------------------
	//! Returns true and fills raw when a new value must be applied for this key.
	protected bool FetchRaw(string fullKey, out string raw)
	{
		if (m_eMode == ARIA_EConfigMode.LOAD)
		{
			if (!m_mValues.Find(fullKey, raw))
				return false;

			m_UsedKeys.Insert(fullKey);
			return true;
		}

		if (m_eMode == ARIA_EConfigMode.SET_ONE)
		{
			if (fullKey != m_sSetKey)
				return false;

			raw = m_sSetValue;
			m_bSetApplied = true;
			return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	protected void WriteLine(string fullKey, string value, string desc)
	{
		string line = fullKey + " = " + value;
		if (m_bCommentValues)
			line = "#" + line;

		if (!desc.IsEmpty())
			line = line + "    # " + desc;

		m_aLines.Insert(line);
	}

	//------------------------------------------------------------------------------------------------
	float Float(string key, float current, float min, float max, string desc)
	{
		string fullKey = FullKey(key);
		if (m_eMode == ARIA_EConfigMode.SAVE)
		{
			WriteLine(fullKey, current.ToString(), string.Format("%1 (%2..%3)", desc, min, max));
			return current;
		}

		string raw;
		if (!FetchRaw(fullKey, raw))
			return current;

		return Math.Clamp(raw.ToFloat(), min, max);
	}

	//------------------------------------------------------------------------------------------------
	int Int(string key, int current, int min, int max, string desc)
	{
		string fullKey = FullKey(key);
		if (m_eMode == ARIA_EConfigMode.SAVE)
		{
			WriteLine(fullKey, current.ToString(), string.Format("%1 (%2..%3)", desc, min, max));
			return current;
		}

		string raw;
		if (!FetchRaw(fullKey, raw))
			return current;

		return Math.ClampInt(raw.ToInt(), min, max);
	}

	//------------------------------------------------------------------------------------------------
	bool Bool(string key, bool current, string desc)
	{
		string fullKey = FullKey(key);
		if (m_eMode == ARIA_EConfigMode.SAVE)
		{
			string value = "false";
			if (current)
				value = "true";

			WriteLine(fullKey, value, desc + " (true/false)");
			return current;
		}

		string raw;
		if (!FetchRaw(fullKey, raw))
			return current;

		raw.ToLower();
		return raw == "true" || raw == "1" || raw == "yes" || raw == "on";
	}

	//------------------------------------------------------------------------------------------------
	string Str(string key, string current, string desc)
	{
		string fullKey = FullKey(key);
		if (m_eMode == ARIA_EConfigMode.SAVE)
		{
			WriteLine(fullKey, current, desc);
			return current;
		}

		string raw;
		if (!FetchRaw(fullKey, raw))
			return current;

		return raw;
	}

	//------------------------------------------------------------------------------------------------
	//! Parses one line of the config file into m_mValues.
	void ParseLine(string line)
	{
		int hash = line.IndexOf("#");
		if (hash >= 0)
			line = line.Substring(0, hash);

		line = line.Trim();
		if (line.IsEmpty() || line.StartsWith("["))
			return;

		int eq = line.IndexOf("=");
		if (eq <= 0)
			return;

		string key = line.Substring(0, eq).Trim();
		string value = line.Substring(eq + 1, line.Length() - eq - 1).Trim();
		if (!key.IsEmpty())
			m_mValues.Set(key, value);
	}
}
