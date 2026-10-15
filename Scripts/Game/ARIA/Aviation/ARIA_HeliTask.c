// ARIA - helicopter task queue entries.
//
// A helicopter mission is a list of simple tasks executed in order, e.g. an
// airlift is:
//   LAND(pickup) -> BOARD(squad) -> LAND(drop LZ) -> UNLOAD(squad) -> RTB
// and close air support is:
//   ORBIT(target, station time) -> RTB

enum ARIA_EHeliTaskType
{
	MOVE,		//!< fly to a point (pass through or stop in a hover)
	LAND,		//!< find an LZ near the point and land
	BOARD,		//!< wait on the ground for a squad to board
	UNLOAD,		//!< wait on the ground until passengers are out
	ORBIT,		//!< circle a point (door gunners / CAS / overwatch)
	HOVER,		//!< hold a hover over a point for a duration
	RTB			//!< return to the home position and shut down
}

class ARIA_HeliTask
{
	ARIA_EHeliTaskType m_eType;
	vector m_vPosition;
	float m_fDuration = -1;			//!< seconds, -1 = unlimited
	float m_fRadius;				//!< orbit radius / arrival radius
	float m_fAltitude = -1;			//!< AGL override, -1 = use defaults
	bool m_bStopAtTarget = true;	//!< MOVE: hover at the end instead of passing through
	bool m_bRepeat;					//!< re-queued at the end when done (patrol cycles)
	ARIA_GroupBrain m_Squad;		//!< BOARD / UNLOAD squad (weak)
	bool m_bFromAirOps;				//!< created by ARIA_AirOps (support mission)

	// Runtime
	float m_fStartTime = -1;
	bool m_bStarted;

	//------------------------------------------------------------------------------------------------
	static ARIA_HeliTask Create(ARIA_EHeliTaskType type, vector position)
	{
		ARIA_HeliTask task = new ARIA_HeliTask();
		task.m_eType = type;
		task.m_vPosition = position;
		return task;
	}

	//------------------------------------------------------------------------------------------------
	string ToDebugString()
	{
		return typename.EnumToString(ARIA_EHeliTaskType, m_eType) + " @ " + m_vPosition.ToString();
	}
}
