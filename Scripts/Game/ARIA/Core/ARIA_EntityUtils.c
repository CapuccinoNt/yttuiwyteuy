// ARIA - small helpers around characters, factions and damage state.

class ARIA_EntityUtils
{
	//------------------------------------------------------------------------------------------------
	static bool IsDeadOrDestroyed(IEntity entity)
	{
		if (!entity)
			return true;

		ChimeraCharacter character = ChimeraCharacter.Cast(entity);
		if (character)
		{
			CharacterControllerComponent controller = character.GetCharacterController();
			if (controller)
				return controller.IsDead();

			return false;
		}

		DamageManagerComponent damage = DamageManagerComponent.Cast(entity.FindComponent(DamageManagerComponent));
		if (damage)
			return damage.IsDestroyed();

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Alive and conscious (able to act).
	static bool IsAbleCharacter(IEntity entity)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(entity);
		if (!character)
			return false;

		CharacterControllerComponent controller = character.GetCharacterController();
		if (!controller)
			return false;

		return controller.GetLifeState() == ECharacterLifeState.ALIVE;
	}

	//------------------------------------------------------------------------------------------------
	static Faction GetFaction(IEntity entity)
	{
		if (!entity)
			return null;

		return SCR_FactionManager.SGetFaction(entity);
	}

	//------------------------------------------------------------------------------------------------
	static string GetFactionKey(IEntity entity)
	{
		Faction faction = GetFaction(entity);
		if (!faction)
			return string.Empty;

		return faction.GetFactionKey();
	}

	//------------------------------------------------------------------------------------------------
	static bool AreEnemies(Faction a, Faction b)
	{
		if (!a || !b)
			return false;

		return a.IsFactionEnemy(b);
	}

	//------------------------------------------------------------------------------------------------
	static bool AreFriends(Faction a, Faction b)
	{
		if (!a || !b)
			return false;

		return a == b || a.IsFactionFriendly(b);
	}

	//------------------------------------------------------------------------------------------------
	//! Hull health in [0..1], 1 when there is no damage manager.
	static float GetHealthScaled(IEntity entity)
	{
		if (!entity)
			return 0;

		DamageManagerComponent damage = DamageManagerComponent.Cast(entity.FindComponent(DamageManagerComponent));
		if (!damage)
			return 1;

		return damage.GetHealthScaled();
	}

	//------------------------------------------------------------------------------------------------
	static AIAgent GetAgent(IEntity entity)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(entity);
		if (!character)
			return null;

		AIControlComponent control = character.GetAIControlComponent();
		if (!control)
			return null;

		return control.GetControlAIAgent();
	}

	//------------------------------------------------------------------------------------------------
	//! Vehicle the character currently sits in, or null.
	static IEntity GetVehicle(IEntity entity)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(entity);
		if (!character || !character.IsInVehicle())
			return null;

		CompartmentAccessComponent access = character.GetCompartmentAccessComponent();
		if (!access)
			return null;

		// Turret / cargo-frame seats live on child entities: report the vehicle itself
		BaseCompartmentSlot slot = access.GetCompartment();
		if (slot)
			return slot.GetVehicle();

		return access.GetVehicleCompartmentManagerOwner();
	}

	//------------------------------------------------------------------------------------------------
	static BaseCompartmentSlot GetCompartment(IEntity entity)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(entity);
		if (!character)
			return null;

		CompartmentAccessComponent access = character.GetCompartmentAccessComponent();
		if (!access || !access.IsInCompartment())
			return null;

		return access.GetCompartment();
	}

	//------------------------------------------------------------------------------------------------
	//! Maps vanilla perception unit types to ARIA contact types.
	static ARIA_EContactType ContactTypeFromUnitType(EAIUnitType unitType)
	{
		switch (unitType)
		{
			case EAIUnitType.UnitType_VehicleUnarmored:
				return ARIA_EContactType.SOFT_VEHICLE;
			case EAIUnitType.UnitType_VehicleMedium:
				return ARIA_EContactType.ARMOR;
			case EAIUnitType.UnitType_VehicleHeavy:
				return ARIA_EContactType.ARMOR;
			case EAIUnitType.UnitType_Aircraft:
				return ARIA_EContactType.AIR;
			case EAIUnitType.UnitType_Fortification:
				return ARIA_EContactType.STATIC;
		}

		return ARIA_EContactType.INFANTRY;
	}

	//------------------------------------------------------------------------------------------------
	static ARIA_EContactType GetContactType(IEntity entity)
	{
		if (!entity)
			return ARIA_EContactType.INFANTRY;

		PerceivableComponent perceivable = PerceivableComponent.Cast(entity.FindComponent(PerceivableComponent));
		if (perceivable)
			return ContactTypeFromUnitType(perceivable.GetUnitType());

		return ARIA_EContactType.INFANTRY;
	}
}
