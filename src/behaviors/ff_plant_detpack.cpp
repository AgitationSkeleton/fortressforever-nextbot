// ff-nextbot: demomen plant detpacks on detpack targets (see ff_plant_detpack.h)
#include "cbase.h"
#include "ff_plant_detpack.h"
#include "game/ff_census.h"
#include "nav_mesh.h"
#include "tf_nav_mesh.h"

extern bool FF_AreTeamsFriendly(int a, int b);
static const int FF_AMMO_DETPACK = 5;   // FF GetAmmoDef order (see shim/pf2_tf.cpp)

ConVar ff_bot_detpack("ff_bot_detpack", "1", FCVAR_NONE, "Demomen plant detpacks on the map's detpack targets (walls, command centres)");

// when a target was last detpacked (by any bot): leave it alone for a while, it is probably open
static float s_lastPlanted[MAX_EDICTS];

static bool HasDetpack(CTFBot *me)
{
	return me->CBaseCombatCharacter::GetAmmoCount(FF_AMMO_DETPACK) > 0;
}

static bool IsMyTarget(CTFBot *me, const ff::CensusObj *o)
{
	if (!o->detTarget || o->disabled)
		return false;
	if (o->ent > 0 && o->ent < MAX_EDICTS && s_lastPlanted[o->ent] > 0.0f && gpGlobals->curtime - s_lastPlanted[o->ent] < 90.0f &&
		gpGlobals->curtime >= s_lastPlanted[o->ent])
		return false;
	// a team's trigger reacts to its enemies' detpacks; teamless ones (team 0/-1/1) to anyone's
	return o->team < FIRST_GAME_TEAM || !FF_AreTeamsFriendly(me->GetTeamNumber(), o->team);
}

static int NearestTarget(CTFBot *me)
{
	int best = 0;
	float bestDist = FLT_MAX;
	for (int i = 0; i < ff::Census_Count(); ++i)
	{
		const ff::CensusObj *o = ff::Census_At(i);
		if (!IsMyTarget(me, o))
			continue;
		CBaseEntity *e = PF2_EntityByIndex(o->ent);
		if (!e)
			continue;
		float d = (e->WorldSpaceCenter() - me->GetAbsOrigin()).LengthSqr();
		if (d < bestDist)
		{
			bestDist = d;
			best = o->ent;
		}
	}
	return best;
}

bool CTFBotFFPlantDetpack::IsPossible(CTFBot *me)
{
	return ff_bot_detpack.GetBool() && me->IsPlayerClass(TF_CLASS_DEMOMAN) && HasDetpack(me) && NearestTarget(me) != 0;
}

ActionResult<CTFBot> CTFBotFFPlantDetpack::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_target = NearestTarget(me);
	if (!m_target)
		return Done("No detpack target");
	m_planted = false;
	m_pathFailures = 0;
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	m_repathTimer.Invalidate();
	m_giveUpTimer.Start(60.0f);
	return Continue();
}

ActionResult<CTFBot> CTFBotFFPlantDetpack::Update(CTFBot *me, float interval)
{
	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if (threat)
		me->EquipBestWeaponForThreat(threat);

	if (m_planted)
	{
		// get clear of the blast: keep following the escape path until the fuse is nearly done
		if (m_fleeTimer.IsElapsed())
			return Done("Detpack planted");
		m_path.Update(me);
		return Continue();
	}

	if (!HasDetpack(me))
		return Done("No detpack");
	if (m_giveUpTimer.IsElapsed())
		return Done("Could not reach the detpack target");
	CBaseEntity *target = PF2_EntityByIndex(m_target);
	if (!target)
		return Done("Detpack target is gone");

	Vector mins, maxs;
	target->CollisionProp()->WorldSpaceAABB(&mins, &maxs);
	Vector feet = me->GetAbsOrigin();
	bool inside = feet.x >= mins.x && feet.x <= maxs.x && feet.y >= mins.y && feet.y <= maxs.y &&
		feet.z + 36.0f >= mins.z && feet.z <= maxs.z;
	if (inside && me->GetLocomotionInterface()->IsOnGround())
	{
		// "detpack <fuse>": FF places it at our feet after a short build time; we must stand still
		me->ClientCommand("detpack 5");
		if (m_target > 0 && m_target < MAX_EDICTS)
			s_lastPlanted[m_target] = gpGlobals->curtime;
		extern void PF2NB_Log(const char *fmt, ...);
		PF2NB_Log("%s planted a detpack in '%s' at (%.0f %.0f %.0f)", me->GetPlayerName(), STRING(target->GetEntityName()), feet.x, feet.y, feet.z);
		m_planted = true;
		m_detpackSpot = feet;
		m_fleeTimer.Start(6.0f);
		// run to the reachable spot farthest from the detpack within a short walk
		CUtlVector<CNavArea *> areas;
		CNavArea *start = me->GetLastKnownArea();
		if (start)
			CollectSurroundingAreas(&areas, start, 1500.0f, StepHeight, StepHeight);
		CNavArea *far = NULL;
		float farDist = 0.0f;
		for (CNavArea *a : areas)
		{
			float d = (a->GetCenter() - m_detpackSpot).LengthSqr();
			if (d > farDist)
			{
				farDist = d;
				far = a;
			}
		}
		if (far)
		{
			CTFBotPathCost cost(me, FASTEST_ROUTE);
			m_path.Compute(me, far->GetCenter(), cost);
		}
		return Continue();
	}

	if (m_repathTimer.IsElapsed())
	{
		m_repathTimer.Start(RandomFloat(1.0f, 2.0f));
		Vector goal = target->WorldSpaceCenter();
		goal.z = mins.z + 8.0f;   // the floor inside the trigger
		CTFBotPathCost cost(me, DEFAULT_ROUTE);
		if (!m_path.Compute(me, goal, cost))
		{
			if (++m_pathFailures >= 3)
				return Done("Detpack target unreachable");
		}
		else
			m_pathFailures = 0;
	}
	if (inside)
		me->GetLocomotionInterface()->Stop();   // in the air inside the zone: land, then plant
	else
		m_path.Update(me);
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotFFPlantDetpack::OnStuck(CTFBot *me)
{
	m_repathTimer.Invalidate();
	me->GetLocomotionInterface()->Jump();
	return TryContinue();
}
