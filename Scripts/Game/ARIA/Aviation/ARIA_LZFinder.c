// ARIA - landing zone search.
//
// Spirals outwards from the requested point and accepts the first spot that is
// flat enough, dry, and free of trees/buildings inside the rotor clearance
// radius. The search is incremental (a few candidates per call) so even a big
// search never causes a frame spike on the server.

class ARIA_LZSearch
{
	protected vector m_vCenter;
	protected float m_fMaxRadius;
	protected float m_fClearRadius;
	protected float m_fMaxSlope;
	protected IEntity m_Exclude;

	protected int m_iRing;
	protected int m_iIndex;
	protected bool m_bDone;
	protected bool m_bFound;
	protected vector m_vResult;

	protected static const float RING_STEP = 20;
	protected static const int CANDIDATES_PER_STEP = 3;

	//------------------------------------------------------------------------------------------------
	void ARIA_LZSearch(vector center, float maxRadius, float clearRadius, float maxSlopeDeg, IEntity exclude)
	{
		m_vCenter = center;
		m_fMaxRadius = maxRadius;
		m_fClearRadius = clearRadius;
		m_fMaxSlope = maxSlopeDeg;
		m_Exclude = exclude;
	}

	//------------------------------------------------------------------------------------------------
	bool IsDone()
	{
		return m_bDone;
	}

	//------------------------------------------------------------------------------------------------
	bool IsFound()
	{
		return m_bFound;
	}

	//------------------------------------------------------------------------------------------------
	//! Best LZ found, or the requested centre when nothing suitable exists.
	vector GetResult()
	{
		if (m_bFound)
			return m_vResult;

		return ARIA_WorldQuery.SnapToGround(m_vCenter);
	}

	//------------------------------------------------------------------------------------------------
	//! Evaluates a few candidates. Returns true when the search is finished.
	bool Step()
	{
		if (m_bDone)
			return true;

		for (int n = 0; n < CANDIDATES_PER_STEP; n++)
		{
			vector candidate;
			if (!NextCandidate(candidate))
			{
				m_bDone = true;
				return true;
			}

			if (IsSuitable(candidate))
			{
				m_vResult = ARIA_WorldQuery.SnapToGround(candidate);
				m_bFound = true;
				m_bDone = true;
				return true;
			}
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Synchronous variant for small searches.
	vector Run()
	{
		while (!Step())
		{
		}

		return GetResult();
	}

	//------------------------------------------------------------------------------------------------
	protected bool NextCandidate(out vector candidate)
	{
		float radius = m_iRing * RING_STEP;
		if (radius > m_fMaxRadius)
			return false;

		// Ring 0 is the centre itself; ring n has 6*n samples
		int samples = Math.Max(1, m_iRing * 6);
		float yaw = (360.0 / samples) * m_iIndex + m_iRing * 17;
		candidate = m_vCenter + ARIA_Math.DirFromYaw(yaw) * radius;

		m_iIndex++;
		if (m_iIndex >= samples)
		{
			m_iIndex = 0;
			m_iRing++;
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsSuitable(vector candidate)
	{
		if (ARIA_WorldQuery.IsWater(candidate))
			return false;

		if (ARIA_WorldQuery.GetSlopeDeg(candidate, m_fClearRadius * 0.6) > m_fMaxSlope)
			return false;

		return ARIA_WorldQuery.IsAreaClear(candidate, m_fClearRadius, 1.2, m_Exclude);
	}
}
