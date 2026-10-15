// ARIA - tactical position planning (flank, withdraw, overwatch, assault).
//
// All functions are static and only use ARIA_WorldQuery for terrain and
// line-of-sight, so the decision logic stays readable and testable.

class ARIA_TacticsPlanner
{
	protected static const float EYE_HEIGHT = 1.6;

	//------------------------------------------------------------------------------------------------
	//! Picks the better flank side and returns a position on the enemy's flank.
	//! Score prefers routes hidden from the enemy, slightly higher ground, no water.
	//! \param[out] flankPos final assault position on the enemy's side
	//! \param[out] stagingPos intermediate point to reach before turning in
	static bool PlanFlank(vector ownPos, vector enemyPos, float minDist, float maxDist, out vector flankPos, out vector stagingPos)
	{
		vector axis = ARIA_Math.Flatten(enemyPos - ownPos);
		float dist = axis.Length();
		if (dist < minDist || dist > maxDist)
			return false;

		vector dirToEnemy = axis * (1 / dist);
		vector right = ARIA_Math.RightOf(dirToEnemy);
		float flankRadius = Math.Clamp(dist * 0.55, 60, 260);

		float bestScore = -float.MAX;
		bool found = false;

		// Two sides x two depths: shallow (~70 deg) and deep (~100 deg) flanks
		for (int side = -1; side <= 1; side += 2)
		{
			for (int depth = 0; depth < 2; depth++)
			{
				float angle = 70 + depth * 30;
				float rad = angle * Math.DEG2RAD;
				// Rotate the "enemy -> us" vector by +-angle around the enemy
				vector fromEnemy = dirToEnemy * -Math.Cos(rad) + right * (side * Math.Sin(rad));
				vector candidate = enemyPos + fromEnemy * flankRadius;
				candidate = ARIA_WorldQuery.SnapToGround(candidate);

				if (ARIA_WorldQuery.IsWater(candidate))
					continue;

				// Staging point: offset sideways from our position, 40% of the way
				vector staging = ownPos + dirToEnemy * (dist * 0.4) + right * (side * flankRadius * 0.8);
				staging = ARIA_WorldQuery.SnapToGround(staging);
				if (ARIA_WorldQuery.IsWater(staging))
					continue;

				float score = ScoreRoute(ownPos, staging, candidate, enemyPos);
				// Small random factor so squads are not predictable
				score += Math.RandomFloat(0, 0.6);

				if (score > bestScore)
				{
					bestScore = score;
					flankPos = candidate;
					stagingPos = staging;
					found = true;
				}
			}
		}

		return found;
	}

	//------------------------------------------------------------------------------------------------
	//! Higher score = better concealed and safer route.
	protected static float ScoreRoute(vector start, vector staging, vector end, vector enemyPos)
	{
		vector enemyEye = enemyPos + vector.Up * EYE_HEIGHT;
		float score = 0;

		// Sample the route; every hidden sample adds to the score
		for (int i = 1; i <= 4; i++)
		{
			float t = i / 4.0;
			vector p;
			if (t <= 0.5)
				p = vector.Lerp(start, staging, t * 2);
			else
				p = vector.Lerp(staging, end, (t - 0.5) * 2);

			p = ARIA_WorldQuery.SnapToGround(p) + vector.Up * 1.0;
			if (!ARIA_WorldQuery.HasLineOfSight(enemyEye, p, null))
				score += 1.0;
		}

		// Slight preference for higher ground at the assault position
		float heightAdvantage = end[1] - enemyPos[1];
		score += Math.Clamp(heightAdvantage / 20.0, -1, 1);

		// Shorter routes are better
		float routeLength = vector.Distance(start, staging) + vector.Distance(staging, end);
		score -= routeLength / 1000.0;
		return score;
	}

	//------------------------------------------------------------------------------------------------
	//! Picks a fallback position away from the enemy, preferring positions out of sight.
	static vector PlanWithdraw(vector ownPos, vector enemyPos, float distance)
	{
		vector away = ARIA_Math.SafeNormalize(ARIA_Math.Flatten(ownPos - enemyPos), vector.Forward);
		float baseYaw = ARIA_Math.YawFromDir(away);
		vector enemyEye = enemyPos + vector.Up * EYE_HEIGHT;

		vector best = ownPos + away * distance;
		float bestScore = -float.MAX;
		for (int i = -2; i <= 2; i++)
		{
			float yaw = baseYaw + i * 25;
			vector candidate = ownPos + ARIA_Math.DirFromYaw(yaw) * distance;
			candidate = ARIA_WorldQuery.SnapToGround(candidate);
			if (ARIA_WorldQuery.IsWater(candidate))
				continue;

			float score = 0;
			if (!ARIA_WorldQuery.HasLineOfSight(enemyEye, candidate + vector.Up * 1.2, null))
				score += 2;

			score -= Math.AbsFloat(i) * 0.3; // prefer straight away from the enemy
			score += Math.RandomFloat(0, 0.3);
			if (score > bestScore)
			{
				bestScore = score;
				best = candidate;
			}
		}

		return best;
	}

	//------------------------------------------------------------------------------------------------
	//! Position with line of sight to the enemy, at standoff distance, for fire support
	//! (armed vehicles, machine gunners, overwatch elements).
	static vector PlanOverwatch(vector ownPos, vector enemyPos, float standoff)
	{
		vector fromEnemy = ARIA_Math.SafeNormalize(ARIA_Math.Flatten(ownPos - enemyPos), vector.Forward);
		float baseYaw = ARIA_Math.YawFromDir(fromEnemy);
		vector enemyTarget = enemyPos + vector.Up * 1.0;

		vector best = enemyPos + fromEnemy * standoff;
		float bestScore = -float.MAX;
		for (int i = -3; i <= 3; i++)
		{
			float yaw = baseYaw + i * 15;
			vector candidate = ARIA_WorldQuery.SnapToGround(enemyPos + ARIA_Math.DirFromYaw(yaw) * standoff);
			if (ARIA_WorldQuery.IsWater(candidate))
				continue;

			float score = 0;
			if (ARIA_WorldQuery.HasLineOfSight(candidate + vector.Up * 2.5, enemyTarget, null))
				score += 3;

			score += Math.Clamp((candidate[1] - enemyPos[1]) / 15.0, -1, 2);
			score -= vector.Distance(candidate, ownPos) / 400.0;
			if (score > bestScore)
			{
				bestScore = score;
				best = candidate;
			}
		}

		return best;
	}

	//------------------------------------------------------------------------------------------------
	//! Point between us and the enemy where reinforcements should join the fight.
	static vector PlanReinforcementPoint(vector helperPos, vector friendlyPos, vector enemyPos)
	{
		// Arrive beside the friendly squad, slightly offset towards our approach side,
		// so reinforcements extend the line instead of walking into the enemy.
		vector enemyDir = ARIA_Math.SafeNormalize(ARIA_Math.Flatten(enemyPos - friendlyPos), vector.Forward);
		vector right = ARIA_Math.RightOf(enemyDir);
		float side = 1;
		if (ARIA_Math.Dot(helperPos - friendlyPos, right) < 0)
			side = -1;

		vector p = friendlyPos + right * (side * 60) - enemyDir * 20;
		return ARIA_WorldQuery.SnapToGround(p);
	}
}
