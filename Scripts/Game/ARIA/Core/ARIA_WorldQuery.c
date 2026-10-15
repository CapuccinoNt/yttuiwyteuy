// ARIA - terrain, water, line-of-sight and obstacle queries.
// Thin static wrapper around BaseWorld so that tactics/aviation code reads
// like plain English and trace setup lives in exactly one place.

//! Trace filter that ignores an entity and everything attached to it
//! (rotors, turrets, crew and passengers of a vehicle).
class ARIA_TraceFilter
{
	IEntity m_IgnoreRoot;

	//------------------------------------------------------------------------------------------------
	bool Filter(notnull IEntity e, vector start = "0 0 0", vector dir = "0 0 0")
	{
		if (!m_IgnoreRoot)
			return true;

		if (e == m_IgnoreRoot || e.GetRootParent() == m_IgnoreRoot)
			return false;

		// Crew and passengers sitting in the vehicle
		if (ChimeraCharacter.Cast(e) && CompartmentAccessComponent.GetVehicleIn(e) == m_IgnoreRoot)
			return false;

		return true;
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_WorldQuery
{
	protected static ref TraceParam s_Trace;
	protected static ref ARIA_TraceFilter s_Filter;

	//------------------------------------------------------------------------------------------------
	static BaseWorld GetWorld()
	{
		return GetGame().GetWorld();
	}

	//------------------------------------------------------------------------------------------------
	protected static TraceParam PrepareTrace(vector from, vector to, IEntity exclude, TraceFlags flags)
	{
		if (!s_Trace)
			s_Trace = new TraceParam();

		if (!s_Filter)
			s_Filter = new ARIA_TraceFilter();

		s_Filter.m_IgnoreRoot = exclude;

		s_Trace.Start = from;
		s_Trace.End = to;
		s_Trace.Flags = flags;
		s_Trace.LayerMask = EPhysicsLayerPresets.Projectile;
		s_Trace.Exclude = exclude;
		s_Trace.ExcludeArray = null;
		s_Trace.TraceEnt = null;
		return s_Trace;
	}

	//------------------------------------------------------------------------------------------------
	//! Terrain height at x/z. On maps with an ocean, never lower than the water surface.
	static float GetSurfaceY(vector pos)
	{
		BaseWorld world = GetWorld();
		float y = world.GetSurfaceY(pos[0], pos[2]);
		if (world.IsOcean())
			y = Math.Max(y, world.GetOceanBaseHeight());

		return y;
	}

	//------------------------------------------------------------------------------------------------
	static vector SnapToGround(vector pos)
	{
		return Vector(pos[0], GetSurfaceY(pos), pos[2]);
	}

	//------------------------------------------------------------------------------------------------
	//! True when the terrain at pos is below the ocean surface.
	static bool IsWater(vector pos)
	{
		BaseWorld world = GetWorld();
		if (!world.IsOcean())
			return false;

		return world.GetSurfaceY(pos[0], pos[2]) < world.GetOceanBaseHeight() + 0.2;
	}

	//------------------------------------------------------------------------------------------------
	//! Line of sight between two points against terrain and objects.
	static bool HasLineOfSight(vector from, vector to, IEntity exclude)
	{
		TraceParam trace = PrepareTrace(from, to, exclude, TraceFlags.WORLD | TraceFlags.ENTS | TraceFlags.ANY_CONTACT);
		float fraction = GetWorld().TraceMove(trace, s_Filter.Filter);
		return fraction >= 0.999;
	}

	//------------------------------------------------------------------------------------------------
	//! Fraction [0..1] of the segment that is free, and the hit entity (if any).
	static float TraceSegment(vector from, vector to, IEntity exclude, out IEntity hitEntity)
	{
		TraceParam trace = PrepareTrace(from, to, exclude, TraceFlags.WORLD | TraceFlags.ENTS);
		float fraction = GetWorld().TraceMove(trace, s_Filter.Filter);
		hitEntity = trace.TraceEnt;
		return fraction;
	}

	//------------------------------------------------------------------------------------------------
	//! Height of the highest obstacle (trees, buildings, terrain) at x/z, probing down from probeTop.
	static float GetObstacleTopY(vector pos, float probeTop, IEntity exclude)
	{
		float ground = GetSurfaceY(pos);
		vector from = Vector(pos[0], probeTop, pos[2]);
		vector to = Vector(pos[0], ground - 1, pos[2]);
		TraceParam trace = PrepareTrace(from, to, exclude, TraceFlags.WORLD | TraceFlags.ENTS);
		float fraction = GetWorld().TraceMove(trace, s_Filter.Filter);
		if (fraction >= 1)
			return ground;

		float hitY = from[1] + (to[1] - from[1]) * fraction;
		return Math.Max(hitY, ground);
	}

	//------------------------------------------------------------------------------------------------
	//! Max terrain slope in degrees inside a radius (4 cross samples + centre).
	static float GetSlopeDeg(vector center, float radius)
	{
		float c = GetSurfaceY(center);
		float maxDiff = 0;
		for (int i = 0; i < 4; i++)
		{
			vector p = center + ARIA_Math.DirFromYaw(i * 90) * radius;
			float diff = Math.AbsFloat(GetSurfaceY(p) - c);
			if (diff > maxDiff)
				maxDiff = diff;
		}

		return Math.Atan2(maxDiff, radius) * Math.RAD2DEG;
	}

	//------------------------------------------------------------------------------------------------
	//! True when no object rises more than `tolerance` meters above the terrain inside radius.
	static bool IsAreaClear(vector center, float radius, float tolerance, IEntity exclude)
	{
		float probeTop = GetSurfaceY(center) + 60;
		if (GetObstacleTopY(center, probeTop, exclude) - GetSurfaceY(center) > tolerance)
			return false;

		// Two rings of samples: half radius and full radius
		for (int ring = 1; ring <= 2; ring++)
		{
			float r = radius * ring * 0.5;
			for (int i = 0; i < 8; i++)
			{
				vector p = center + ARIA_Math.DirFromYaw(i * 45 + ring * 22.5) * r;
				if (IsWater(p))
					return false;

				float top = GetObstacleTopY(p, probeTop, exclude);
				if (top - GetSurfaceY(p) > tolerance)
					return false;
			}
		}

		return true;
	}
}
