// ARIA - turns vanilla waypoints into helicopter tasks, so mission makers and
// Game Masters use the waypoints they already know:
//
//   Move / Forced Move / Patrol   -> fly there (last one: land & unload if carrying troops)
//   Cycle                         -> fly the patrol loop forever
//   Defend                        -> orbit the area (overwatch)
//   Search & Destroy / Attack /
//   Suppress                      -> close air support orbit for casStationTime
//   Get Out                       -> land at the nearest safe LZ and unload passengers
//   Get In                        -> land and wait for passengers to board
//   Wait / Loiter / timed         -> hover there for the holding time

class ARIA_HeliWaypointTranslator
{
	//------------------------------------------------------------------------------------------------
	static void Translate(AIWaypoint waypoint, bool isLast, bool hasPassengers, ARIA_HeliSettings cfg, notnull array<ref ARIA_HeliTask> outTasks)
	{
		if (!waypoint)
			return;

		vector pos = waypoint.GetOrigin();

		AIWaypointCycle cycle = AIWaypointCycle.Cast(waypoint);
		if (cycle)
		{
			array<AIWaypoint> inner = {};
			cycle.GetWaypoints(inner);
			foreach (AIWaypoint innerWaypoint : inner)
			{
				if (!innerWaypoint)
					continue;

				ARIA_HeliTask leg = ARIA_HeliTask.Create(ARIA_EHeliTaskType.MOVE, innerWaypoint.GetOrigin());
				leg.m_bStopAtTarget = false;
				leg.m_bRepeat = true;
				leg.m_fRadius = Math.Max(innerWaypoint.GetCompletionRadius(), 80);
				outTasks.Insert(leg);
			}
			return;
		}

		string kind = GetKind(waypoint);

		if (SCR_DefendWaypoint.Cast(waypoint) || kind.Contains("defend"))
		{
			ARIA_HeliTask overwatch = ARIA_HeliTask.Create(ARIA_EHeliTaskType.ORBIT, pos);
			overwatch.m_fRadius = Math.Max(waypoint.GetCompletionRadius(), 250);
			overwatch.m_fAltitude = cfg.m_fCombatAltitude;
			outTasks.Insert(overwatch);
			return;
		}

		if (SCR_SearchAndDestroyWaypoint.Cast(waypoint) || kind.Contains("searchanddestroy") || kind.Contains("attack") || kind.Contains("suppress"))
		{
			ARIA_HeliTask cas = ARIA_HeliTask.Create(ARIA_EHeliTaskType.ORBIT, pos);
			cas.m_fRadius = cfg.m_fCASOrbitRadius;
			cas.m_fAltitude = cfg.m_fCASOrbitAltitude;
			cas.m_fDuration = cfg.m_fCASStationTime;
			outTasks.Insert(cas);
			return;
		}

		if (kind.Contains("getout") || kind.Contains("unload"))
		{
			outTasks.Insert(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, pos));
			ARIA_HeliTask unload = ARIA_HeliTask.Create(ARIA_EHeliTaskType.UNLOAD, pos);
			unload.m_fDuration = cfg.m_fUnloadTimeout;
			outTasks.Insert(unload);
			return;
		}

		if (kind.Contains("getin") || kind.Contains("boarding"))
		{
			outTasks.Insert(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, pos));
			ARIA_HeliTask board = ARIA_HeliTask.Create(ARIA_EHeliTaskType.BOARD, pos);
			board.m_fDuration = cfg.m_fBoardTimeout;
			outTasks.Insert(board);
			return;
		}

		SCR_TimedWaypoint timed = SCR_TimedWaypoint.Cast(waypoint);
		if (timed || kind.Contains("wait") || kind.Contains("loiter") || kind.Contains("observation"))
		{
			ARIA_HeliTask hover = ARIA_HeliTask.Create(ARIA_EHeliTaskType.HOVER, pos);
			hover.m_fDuration = 120;
			if (timed && timed.GetHoldingTime() > 0)
				hover.m_fDuration = timed.GetHoldingTime();

			outTasks.Insert(hover);
			return;
		}

		// Move / forced move / patrol / anything else
		ARIA_HeliTask move = ARIA_HeliTask.Create(ARIA_EHeliTaskType.MOVE, pos);
		move.m_bStopAtTarget = isLast;
		move.m_fRadius = Math.Max(waypoint.GetCompletionRadius(), 60);
		outTasks.Insert(move);

		if (isLast && hasPassengers)
		{
			outTasks.Insert(ARIA_HeliTask.Create(ARIA_EHeliTaskType.LAND, pos));
			ARIA_HeliTask drop = ARIA_HeliTask.Create(ARIA_EHeliTaskType.UNLOAD, pos);
			drop.m_fDuration = cfg.m_fUnloadTimeout;
			outTasks.Insert(drop);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Lower-case prefab name + class name, used to recognise waypoint types.
	protected static string GetKind(AIWaypoint waypoint)
	{
		string kind = waypoint.ClassName();
		EntityPrefabData data = waypoint.GetPrefabData();
		if (data)
			kind = kind + " " + data.GetPrefabName();

		kind.ToLower();
		return kind;
	}
}
