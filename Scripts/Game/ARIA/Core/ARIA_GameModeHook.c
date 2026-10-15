// ARIA - bootstrap. Hooks into every game mode (Conflict, Game Master,
// Combat Ops, custom scenarios...) without touching any prefab.

modded class SCR_BaseGameMode
{
	//------------------------------------------------------------------------------------------------
	protected override void OnGameStart()
	{
		super.OnGameStart();

		if (IsMaster())
			ARIA_Manager.Start();
	}

	//------------------------------------------------------------------------------------------------
	override void EOnFrame(IEntity owner, float timeSlice)
	{
		super.EOnFrame(owner, timeSlice);

		ARIA_Manager manager = ARIA_Manager.GetInstance();
		if (manager)
			manager.OnFrame(timeSlice);
	}
}

//------------------------------------------------------------------------------------------------
modded class ArmaReforgerScripted
{
	//------------------------------------------------------------------------------------------------
	override void OnGameEnd()
	{
		ARIA_Manager.Shutdown();
		super.OnGameEnd();
	}
}
