// ARIA - vehicle classification, compartments and searches.
// Works with any vanilla or modded vehicle that uses the standard Reforger
// components (compartment managers, vehicle controllers, AI usage component).

class ARIA_VehicleUtils
{
	//! Vehicles recently claimed by a squad, so two squads do not race for the same one.
	//! Claims expire: once boarded, the occupant check keeps the vehicle unavailable anyway.
	protected static ref array<IEntity> s_aClaimedVehicles = {};
	protected static ref array<float> s_aClaimTimes = {};
	protected static const float CLAIM_TTL_MS = 120000;

	//------------------------------------------------------------------------------------------------
	static bool IsHelicopter(IEntity vehicle)
	{
		return vehicle && vehicle.FindComponent(HelicopterControllerComponent) != null;
	}

	//------------------------------------------------------------------------------------------------
	static HelicopterControllerComponent GetHeliController(IEntity vehicle)
	{
		if (!vehicle)
			return null;

		return HelicopterControllerComponent.Cast(vehicle.FindComponent(HelicopterControllerComponent));
	}

	//------------------------------------------------------------------------------------------------
	static VehicleHelicopterSimulation GetHeliSimulation(IEntity vehicle)
	{
		HelicopterControllerComponent controller = GetHeliController(vehicle);
		if (controller)
			return controller.GetSimulation();

		if (!vehicle)
			return null;

		return VehicleHelicopterSimulation.Cast(vehicle.FindComponent(VehicleHelicopterSimulation));
	}

	//------------------------------------------------------------------------------------------------
	static VehicleWheeledSimulation GetWheeledSimulation(IEntity vehicle)
	{
		if (!vehicle)
			return null;

		return VehicleWheeledSimulation.Cast(vehicle.FindComponent(VehicleWheeledSimulation));
	}

	//------------------------------------------------------------------------------------------------
	static SCR_AIVehicleUsageComponent GetUsageComponent(IEntity vehicle)
	{
		if (!vehicle)
			return null;

		return SCR_AIVehicleUsageComponent.Cast(vehicle.FindComponent(SCR_AIVehicleUsageComponent));
	}

	//------------------------------------------------------------------------------------------------
	static bool IsStaticWeapon(IEntity vehicle)
	{
		SCR_AIVehicleUsageComponent usage = GetUsageComponent(vehicle);
		if (usage)
		{
			EAIVehicleType type = usage.GetVehicleType();
			return type == EAIVehicleType.STATIC_WEAPON || type == EAIVehicleType.STATIC_ARTILLERY;
		}

		return vehicle && !vehicle.FindComponent(VehicleControllerComponent);
	}

	//------------------------------------------------------------------------------------------------
	//! Every compartment of the vehicle including those on child entities (turrets).
	static void GetAllCompartments(IEntity vehicle, notnull array<BaseCompartmentSlot> outSlots)
	{
		if (!vehicle)
			return;

		BaseCompartmentManagerComponent manager = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (manager)
		{
			array<BaseCompartmentSlot> slots = {};
			manager.GetCompartments(slots);
			foreach (BaseCompartmentSlot slot : slots)
			{
				if (!outSlots.Contains(slot))
					outSlots.Insert(slot);
			}
		}

		IEntity child = vehicle.GetChildren();
		while (child)
		{
			GetAllCompartments(child, outSlots);
			child = child.GetSibling();
		}
	}

	//------------------------------------------------------------------------------------------------
	static void GetCompartmentsOfType(IEntity vehicle, ECompartmentType type, notnull array<BaseCompartmentSlot> outSlots)
	{
		array<BaseCompartmentSlot> all = {};
		GetAllCompartments(vehicle, all);
		foreach (BaseCompartmentSlot slot : all)
		{
			if (slot.GetType() == type)
				outSlots.Insert(slot);
		}
	}

	//------------------------------------------------------------------------------------------------
	static int CountSeats(IEntity vehicle, bool freeOnly)
	{
		array<BaseCompartmentSlot> all = {};
		GetAllCompartments(vehicle, all);
		int count = 0;
		foreach (BaseCompartmentSlot slot : all)
		{
			if (!slot.IsCompartmentAccessible())
				continue;

			if (freeOnly && slot.GetOccupant())
				continue;

			count++;
		}

		return count;
	}

	//------------------------------------------------------------------------------------------------
	static int CountOccupants(IEntity vehicle)
	{
		array<BaseCompartmentSlot> all = {};
		GetAllCompartments(vehicle, all);
		int count = 0;
		foreach (BaseCompartmentSlot slot : all)
		{
			if (slot.GetOccupant())
				count++;
		}

		return count;
	}

	//------------------------------------------------------------------------------------------------
	static bool HasTurret(IEntity vehicle)
	{
		array<BaseCompartmentSlot> turrets = {};
		GetCompartmentsOfType(vehicle, ECompartmentType.TURRET, turrets);
		return !turrets.IsEmpty();
	}

	//------------------------------------------------------------------------------------------------
	static BaseCompartmentSlot GetFreeSlot(IEntity vehicle, ECompartmentType type)
	{
		array<BaseCompartmentSlot> slots = {};
		GetCompartmentsOfType(vehicle, type, slots);
		foreach (BaseCompartmentSlot slot : slots)
		{
			if (!slot.GetOccupant() && slot.IsCompartmentAccessible())
				return slot;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	static IEntity GetPilot(IEntity vehicle)
	{
		Vehicle v = Vehicle.Cast(vehicle);
		if (v)
			return v.GetPilot();

		array<BaseCompartmentSlot> pilots = {};
		GetCompartmentsOfType(vehicle, ECompartmentType.PILOT, pilots);
		foreach (BaseCompartmentSlot slot : pilots)
		{
			if (slot.GetOccupant())
				return slot.GetOccupant();
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	static bool IsPilotOf(IEntity character, IEntity vehicle)
	{
		BaseCompartmentSlot slot = ARIA_EntityUtils.GetCompartment(character);
		if (!slot || slot.GetType() != ECompartmentType.PILOT)
			return false;

		return slot.GetVehicle() == vehicle;
	}

	//------------------------------------------------------------------------------------------------
	//! Other able occupants of PILOT compartments (co-pilot seats).
	static IEntity FindCopilot(IEntity vehicle, IEntity currentPilot)
	{
		array<BaseCompartmentSlot> pilots = {};
		GetCompartmentsOfType(vehicle, ECompartmentType.PILOT, pilots);
		foreach (BaseCompartmentSlot slot : pilots)
		{
			IEntity occupant = slot.GetOccupant();
			if (occupant && occupant != currentPilot && ARIA_EntityUtils.IsAbleCharacter(occupant))
				return occupant;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	//! Rough combat value of a vehicle in "riflemen" (used for force ratios).
	static float GetCombatValue(IEntity vehicle)
	{
		if (IsHelicopter(vehicle))
		{
			if (HasTurret(vehicle))
				return 4;

			return 0.5;
		}

		bool armed = HasTurret(vehicle);
		Vehicle v = Vehicle.Cast(vehicle);
		bool armored = v && (v.m_eVehicleType & EVehicleType.APC) != 0;
		if (armed && armored)
			return 6;

		if (armed)
			return 3;

		return 0.5;
	}

	//------------------------------------------------------------------------------------------------
	//! Not destroyed, engine working and hull above the bail-out threshold.
	static bool IsDrivable(IEntity vehicle, float minHealth)
	{
		if (!vehicle || ARIA_EntityUtils.IsDeadOrDestroyed(vehicle))
			return false;

		SCR_VehicleDamageManagerComponent damage = SCR_VehicleDamageManagerComponent.Cast(vehicle.FindComponent(SCR_VehicleDamageManagerComponent));
		if (damage && !damage.GetEngineFunctional())
			return false;

		return ARIA_EntityUtils.GetHealthScaled(vehicle) > minHealth;
	}

	//------------------------------------------------------------------------------------------------
	static bool IsUpsideDown(IEntity vehicle)
	{
		vector up = vehicle.GetWorldTransformAxis(1);
		return up[1] < 0.3;
	}

	//------------------------------------------------------------------------------------------------
	static string GetDebugName(IEntity vehicle)
	{
		if (!vehicle)
			return "<null>";

		EntityPrefabData data = vehicle.GetPrefabData();
		if (data)
		{
			string prefab = data.GetPrefabName();
			int slash = prefab.LastIndexOf("/");
			if (slash >= 0)
				return prefab.Substring(slash + 1, prefab.Length() - slash - 1);

			return prefab;
		}

		return vehicle.ClassName();
	}

	//------------------------------------------------------------------------------------------------
	static void Claim(IEntity vehicle)
	{
		PruneClaims();
		if (!vehicle)
			return;

		Unclaim(vehicle);
		s_aClaimedVehicles.Insert(vehicle);
		s_aClaimTimes.Insert(GetGame().GetWorld().GetWorldTime());
	}

	//------------------------------------------------------------------------------------------------
	static void Unclaim(IEntity vehicle)
	{
		int index = s_aClaimedVehicles.Find(vehicle);
		if (index < 0)
			return;

		s_aClaimedVehicles.RemoveOrdered(index);
		s_aClaimTimes.RemoveOrdered(index);
	}

	//------------------------------------------------------------------------------------------------
	static bool IsClaimed(IEntity vehicle)
	{
		PruneClaims();
		return s_aClaimedVehicles.Contains(vehicle);
	}

	//------------------------------------------------------------------------------------------------
	//! Drops deleted vehicles and expired claims.
	protected static void PruneClaims()
	{
		float now = GetGame().GetWorld().GetWorldTime();
		for (int i = s_aClaimedVehicles.Count() - 1; i >= 0; i--)
		{
			if (!s_aClaimedVehicles[i] || now - s_aClaimTimes[i] > CLAIM_TTL_MS)
			{
				s_aClaimedVehicles.RemoveOrdered(i);
				s_aClaimTimes.RemoveOrdered(i);
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Closest unmanned static weapon (MG, launcher on tripod...) that a squad of `faction` may use.
	static IEntity FindFreeStaticWeapon(vector center, float radius, Faction faction)
	{
		ARIA_StaticWeaponQuery query = new ARIA_StaticWeaponQuery();
		GetGame().GetWorld().QueryEntitiesBySphere(center, radius, query.OnEntity, null, EQueryEntitiesFlags.ALL);

		IEntity best;
		float bestDistSq = float.MAX;
		foreach (IEntity candidate : query.m_aFound)
		{
			if (IsClaimed(candidate) || ARIA_EntityUtils.IsDeadOrDestroyed(candidate))
				continue;

			if (!GetFreeSlot(candidate, ECompartmentType.TURRET))
				continue;

			FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(candidate.FindComponent(FactionAffiliationComponent));
			if (affiliation)
			{
				Faction owner = affiliation.GetAffiliatedFaction();
				if (owner && faction && owner != faction && !faction.IsFactionFriendly(owner))
					continue;
			}

			float d = vector.DistanceSq(center, candidate.GetOrigin());
			if (d < bestDistSq)
			{
				bestDistSq = d;
				best = candidate;
			}
		}

		if (best)
			Claim(best);

		return best;
	}

	//------------------------------------------------------------------------------------------------
	//! Closest empty, drivable, friendly-or-neutral ground vehicle with enough seats.
	static IEntity FindFreeVehicle(vector center, float radius, Faction faction, int seatsNeeded)
	{
		ARIA_VehicleQuery query = new ARIA_VehicleQuery();
		GetGame().GetWorld().QueryEntitiesBySphere(center, radius, query.OnEntity, null, EQueryEntitiesFlags.DYNAMIC);

		IEntity best;
		float bestDistSq = float.MAX;
		foreach (IEntity candidate : query.m_aFound)
		{
			if (!IsUsableFreeVehicle(candidate, faction, seatsNeeded))
				continue;

			float d = vector.DistanceSq(center, candidate.GetOrigin());
			if (d < bestDistSq)
			{
				bestDistSq = d;
				best = candidate;
			}
		}

		if (best)
			Claim(best);

		return best;
	}

	//------------------------------------------------------------------------------------------------
	protected static bool IsUsableFreeVehicle(IEntity vehicle, Faction faction, int seatsNeeded)
	{
		if (IsClaimed(vehicle) || IsHelicopter(vehicle) || ARIA_EntityUtils.IsDeadOrDestroyed(vehicle))
			return false;

		// Never pick up a vehicle that ARIA itself would bail out of
		ARIA_Manager manager = ARIA_Manager.GetInstance();
		float minHealth = 0.25;
		if (manager)
			minHealth = manager.GetSettings().m_Vehicles.m_fBailOutHealth;

		if (!IsDrivable(vehicle, minHealth))
			return false;

		SCR_AIVehicleUsageComponent usage = GetUsageComponent(vehicle);
		if (!usage || !usage.CanBePiloted())
			return false;

		EAIVehicleType type = usage.GetVehicleType();
		if (type != EAIVehicleType.GROUND_VEHICLE_WHEELED && type != EAIVehicleType.GROUND_VEHICLE_TRACKED)
			return false;

		if (CountOccupants(vehicle) > 0 || IsUpsideDown(vehicle))
			return false;

		Vehicle v = Vehicle.Cast(vehicle);
		if (v)
		{
			Faction owner = v.GetFaction();
			if (owner && faction && owner != faction && !faction.IsFactionFriendly(owner))
				return false;
		}

		return CountSeats(vehicle, true) >= seatsNeeded;
	}
}

//------------------------------------------------------------------------------------------------
//! Collects static weapons (AI usage type STATIC_WEAPON) from a world query.
class ARIA_StaticWeaponQuery
{
	ref array<IEntity> m_aFound = {};

	//------------------------------------------------------------------------------------------------
	bool OnEntity(IEntity entity)
	{
		SCR_AIVehicleUsageComponent usage = ARIA_VehicleUtils.GetUsageComponent(entity);
		if (usage && usage.GetVehicleType() == EAIVehicleType.STATIC_WEAPON && !m_aFound.Contains(entity))
			m_aFound.Insert(entity);

		return true;
	}
}

//------------------------------------------------------------------------------------------------
//! Collects Vehicle entities from a world query.
class ARIA_VehicleQuery
{
	ref array<IEntity> m_aFound = {};

	//------------------------------------------------------------------------------------------------
	bool OnEntity(IEntity entity)
	{
		if (Vehicle.Cast(entity) && !m_aFound.Contains(entity))
			m_aFound.Insert(entity);

		return true;
	}
}
