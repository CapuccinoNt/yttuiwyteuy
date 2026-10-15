// ARIA - logging helpers. All output is prefixed with [ARIA] so it can be
// filtered in the Workbench console or server console.log.

enum ARIA_ELogLevel
{
	ERROR,
	WARNING,
	INFO,
	DEBUG,
	VERBOSE
}

class ARIA_Log
{
	//! Runtime log verbosity, set from ARIA_Settings on load.
	static ARIA_ELogLevel s_eLevel = ARIA_ELogLevel.INFO;

	//------------------------------------------------------------------------------------------------
	static void Error(string msg)
	{
		Print("[ARIA] " + msg, LogLevel.ERROR);
	}

	//------------------------------------------------------------------------------------------------
	static void Warning(string msg)
	{
		if (s_eLevel >= ARIA_ELogLevel.WARNING)
			Print("[ARIA] " + msg, LogLevel.WARNING);
	}

	//------------------------------------------------------------------------------------------------
	static void Info(string msg)
	{
		if (s_eLevel >= ARIA_ELogLevel.INFO)
			Print("[ARIA] " + msg, LogLevel.NORMAL);
	}

	//------------------------------------------------------------------------------------------------
	static void Debug(string msg)
	{
		if (s_eLevel >= ARIA_ELogLevel.DEBUG)
			Print("[ARIA] " + msg, LogLevel.DEBUG);
	}

	//------------------------------------------------------------------------------------------------
	static void Verbose(string msg)
	{
		if (s_eLevel >= ARIA_ELogLevel.VERBOSE)
			Print("[ARIA] " + msg, LogLevel.SPAM);
	}
}
