// ARIA - radio network between friendly squads.
//
//  * Contact reports: a squad that spots an enemy radios its position to
//    friendly squads in range, after a configurable radio delay. Receivers add
//    it to their own memory AND to vanilla group perception, so the native AI
//    investigates / suppresses it on its own (LAMBS-style information sharing).
//  * Calls for help: squads in trouble ask idle squads nearby to reinforce,
//    and ask ARIA_AirOps for close air support when armour/large forces show up.

class ARIA_IntelReport
{
	float m_fDeliverTime;
	ARIA_GroupBrain m_Sender;
	ARIA_GroupBrain m_Recipient;
	IEntity m_Enemy;
	vector m_vPosition;
	ARIA_EContactType m_eType;
}

//------------------------------------------------------------------------------------------------
class ARIA_Comms
{
	protected ARIA_Manager m_Manager;
	protected ref array<ref ARIA_IntelReport> m_aPending = {};

	protected static const int MAX_PENDING = 512;

	//------------------------------------------------------------------------------------------------
	void ARIA_Comms(ARIA_Manager manager)
	{
		m_Manager = manager;
	}

	//------------------------------------------------------------------------------------------------
	//! Sender radios one contact to every friendly squad in range.
	void BroadcastContact(ARIA_GroupBrain sender, ARIA_Contact contact, float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		if (!cfg.m_bShareIntel || !contact || !contact.m_Entity)
			return;

		// Teamwork decides whether this squad bothers to report at all
		if (!ARIA_Math.Chance(sender.GetSkill().m_fTeamwork))
			return;

		array<ARIA_GroupBrain> receivers = {};
		m_Manager.FindFriendlyGroups(sender.GetFaction(), sender.GetPosition(), cfg.m_fIntelShareRadius, sender, receivers);
		foreach (ARIA_GroupBrain receiver : receivers)
		{
			if (m_aPending.Count() >= MAX_PENDING)
				break;

			ARIA_IntelReport report = new ARIA_IntelReport();
			report.m_fDeliverTime = now + cfg.m_fIntelRadioDelay * Math.RandomFloat(0.7, 1.3);
			report.m_Sender = sender;
			report.m_Recipient = receiver;
			report.m_Enemy = contact.m_Entity;
			report.m_vPosition = contact.m_vLastKnownPosition;
			report.m_eType = contact.m_eType;
			m_aPending.Insert(report);
		}

		if (!receivers.IsEmpty())
			ARIA_Log.Verbose(string.Format("%1 reports contact to %2 squads", sender.GetDebugName(), receivers.Count()));
	}

	//------------------------------------------------------------------------------------------------
	//! Asks idle friendly squads to reinforce. Returns the number of squads that answered.
	int RequestHelp(ARIA_GroupBrain requester, vector enemyPosition, float enemyStrength, float now)
	{
		ARIA_SquadSettings cfg = m_Manager.GetSettings().m_Squad;
		int answered = 0;

		if (cfg.m_bReinforcements && cfg.m_iMaxReinforcingGroups > 0)
		{
			array<ARIA_GroupBrain> candidates = {};
			m_Manager.FindFriendlyGroups(requester.GetFaction(), requester.GetPosition(), cfg.m_fReinforceRadius, requester, candidates);

			// Closest available squads first
			SortByDistance(candidates, requester.GetPosition());
			foreach (ARIA_GroupBrain candidate : candidates)
			{
				if (answered >= cfg.m_iMaxReinforcingGroups)
					break;

				if (!candidate.CanReinforce())
					continue;

				if (!ARIA_Math.Chance(candidate.GetSkill().m_fTeamwork))
					continue;

				if (candidate.OrderReinforce(requester, enemyPosition, now))
					answered++;
			}
		}

		// Heavy threat: also ask for helicopter support
		ARIA_HeliSettings heli = m_Manager.GetSettings().m_Heli;
		if (heli.m_bEnabled && heli.m_bAutoCAS && enemyStrength >= 6)
			m_Manager.GetAirOps().RequestCAS(requester, enemyPosition, now);

		if (answered > 0)
			ARIA_Log.Debug(string.Format("%1 called for help, %2 squads answered", requester.GetDebugName(), answered));

		return answered;
	}

	//------------------------------------------------------------------------------------------------
	void Update(float now)
	{
		for (int i = m_aPending.Count() - 1; i >= 0; i--)
		{
			ARIA_IntelReport report = m_aPending[i];
			if (report.m_fDeliverTime > now)
				continue;

			if (report.m_Recipient && report.m_Recipient.IsValid() && report.m_Enemy)
				report.m_Recipient.ReceiveIntel(report.m_Enemy, report.m_vPosition, report.m_eType, now);

			m_aPending.Remove(i);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Simple insertion sort, squads lists are short.
	protected void SortByDistance(notnull array<ARIA_GroupBrain> brains, vector from)
	{
		for (int i = 1; i < brains.Count(); i++)
		{
			ARIA_GroupBrain key = brains[i];
			float keyDist = vector.DistanceSq(key.GetPosition(), from);
			int j = i - 1;
			while (j >= 0 && vector.DistanceSq(brains[j].GetPosition(), from) > keyDist)
			{
				brains[j + 1] = brains[j];
				j--;
			}

			brains[j + 1] = key;
		}
	}
}
