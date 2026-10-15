// ARIA - shared squad knowledge about enemies.
//
// Every squad keeps a short-term memory of enemy contacts. Contacts come from
// the squad's own perception (ARIA_GroupBrain.Sense) or from radio reports of
// friendly squads (ARIA_Comms). Memory decays: unseen contacts get less
// certain and are forgotten after ARIA_SquadSettings.m_fContactMemory seconds.

enum ARIA_EContactType
{
	INFANTRY,
	SOFT_VEHICLE,	//!< cars, trucks
	ARMOR,			//!< APC / IFV / tank
	AIR,			//!< helicopters, planes
	STATIC			//!< static weapons, emplacements
}

//------------------------------------------------------------------------------------------------
class ARIA_Contact
{
	IEntity m_Entity;					//!< weak ref, becomes null when deleted
	vector m_vLastKnownPosition;
	float m_fLastSeenTime;				//!< world time (s) when last confirmed
	float m_fFirstSeenTime;
	bool m_bReported;					//!< true when known only from a radio report
	ARIA_EContactType m_eType = ARIA_EContactType.INFANTRY;

	//------------------------------------------------------------------------------------------------
	//! Threat weight used for force ratio estimates (1 = one rifleman).
	float GetThreatWeight()
	{
		switch (m_eType)
		{
			case ARIA_EContactType.SOFT_VEHICLE:
				return 2.0;
			case ARIA_EContactType.ARMOR:
				return 8.0;
			case ARIA_EContactType.AIR:
				return 6.0;
			case ARIA_EContactType.STATIC:
				return 3.0;
		}

		return 1.0;
	}

	//------------------------------------------------------------------------------------------------
	float GetAge(float now)
	{
		return now - m_fLastSeenTime;
	}
}

//------------------------------------------------------------------------------------------------
class ARIA_ThreatMemory
{
	protected ref array<ref ARIA_Contact> m_aContacts = {};

	//------------------------------------------------------------------------------------------------
	array<ref ARIA_Contact> GetContacts()
	{
		return m_aContacts;
	}

	//------------------------------------------------------------------------------------------------
	int Count()
	{
		return m_aContacts.Count();
	}

	//------------------------------------------------------------------------------------------------
	bool IsEmpty()
	{
		return m_aContacts.IsEmpty();
	}

	//------------------------------------------------------------------------------------------------
	ARIA_Contact Find(IEntity entity)
	{
		if (!entity)
			return null;

		foreach (ARIA_Contact contact : m_aContacts)
		{
			if (contact.m_Entity == entity)
				return contact;
		}

		return null;
	}

	//------------------------------------------------------------------------------------------------
	//! Adds or refreshes a contact. Returns true if this is a NEW contact.
	bool Report(IEntity entity, vector position, ARIA_EContactType type, float now, bool viaRadio)
	{
		ARIA_Contact contact = Find(entity);
		if (contact)
		{
			// A radio report never overrides fresher first-hand information
			if (viaRadio && !contact.m_bReported && contact.m_fLastSeenTime >= now - 5)
				return false;

			contact.m_vLastKnownPosition = position;
			contact.m_fLastSeenTime = now;
			contact.m_eType = type;
			if (!viaRadio)
				contact.m_bReported = false;

			return false;
		}

		contact = new ARIA_Contact();
		contact.m_Entity = entity;
		contact.m_vLastKnownPosition = position;
		contact.m_fLastSeenTime = now;
		contact.m_fFirstSeenTime = now;
		contact.m_bReported = viaRadio;
		contact.m_eType = type;
		m_aContacts.Insert(contact);
		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Removes dead/deleted/forgotten contacts.
	void Prune(float now, float memorySeconds)
	{
		for (int i = m_aContacts.Count() - 1; i >= 0; i--)
		{
			ARIA_Contact contact = m_aContacts[i];
			bool remove = !contact.m_Entity || contact.GetAge(now) > memorySeconds;
			if (!remove && ARIA_EntityUtils.IsDeadOrDestroyed(contact.m_Entity))
				remove = true;

			if (remove)
				m_aContacts.Remove(i);
		}
	}

	//------------------------------------------------------------------------------------------------
	void Clear()
	{
		m_aContacts.Clear();
	}

	//------------------------------------------------------------------------------------------------
	//! Sum of threat weights of contacts within range of a position.
	float GetThreatStrength(vector from, float range)
	{
		float sum = 0;
		float rangeSq = range * range;
		foreach (ARIA_Contact contact : m_aContacts)
		{
			if (vector.DistanceSq(from, contact.m_vLastKnownPosition) <= rangeSq)
				sum += contact.GetThreatWeight();
		}

		return sum;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_Contact GetNearest(vector from)
	{
		ARIA_Contact best;
		float bestDistSq = float.MAX;
		foreach (ARIA_Contact contact : m_aContacts)
		{
			float d = vector.DistanceSq(from, contact.m_vLastKnownPosition);
			if (d < bestDistSq)
			{
				bestDistSq = d;
				best = contact;
			}
		}

		return best;
	}

	//------------------------------------------------------------------------------------------------
	ARIA_Contact GetMostRecent()
	{
		ARIA_Contact best;
		float bestTime = -float.MAX;
		foreach (ARIA_Contact contact : m_aContacts)
		{
			if (contact.m_fLastSeenTime > bestTime)
			{
				bestTime = contact.m_fLastSeenTime;
				best = contact;
			}
		}

		return best;
	}

	//------------------------------------------------------------------------------------------------
	bool HasContactOfType(ARIA_EContactType type)
	{
		foreach (ARIA_Contact contact : m_aContacts)
		{
			if (contact.m_eType == type)
				return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Threat-weighted centre of the known enemy positions within range.
	bool GetEnemyCentroid(vector from, float range, out vector centroid)
	{
		float rangeSq = range * range;
		float total = 0;
		vector sum = vector.Zero;
		foreach (ARIA_Contact contact : m_aContacts)
		{
			if (vector.DistanceSq(from, contact.m_vLastKnownPosition) > rangeSq)
				continue;

			float w = contact.GetThreatWeight();
			sum = sum + contact.m_vLastKnownPosition * w;
			total += w;
		}

		if (total <= 0)
			return false;

		centroid = sum * (1 / total);
		return true;
	}
}
