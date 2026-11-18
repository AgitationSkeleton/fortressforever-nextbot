// ff-nextbot: Fortress Forever command points (see ff_command_point.h)
#include "cbase.h"
#include "ff_command_point.h"
#include "game/ff_census.h"
#include "nav_mesh.h"
#include "tf_nav_mesh.h"

extern bool FF_AreTeamsFriendly(int a, int b);

static bool OwnedBySide(const ff::CensusObj *o, int team)
{
	return o->owner >= FIRST_GAME_TEAM && FF_AreTeamsFriendly(team, o->owner);
}

bool CTFBotFFCommandPoint::IsPossible(CTFBot *me)
{
	for (int i = 0; i < ff::Census_Count(); ++i)
		if (ff::Census_At(i)->kind == ff::OBJ_CPZONE && !ff::Census_At(i)->disabled)
			return true;
	return false;
}

int CTFBotFFCommandPoint::ChooseZone(CTFBot *me)
{
	int team = me->GetTeamNumber();
	const Vector &from = me->GetAbsOrigin();
	int bestTake = 0, bestHold = 0;
	float takeDist = FLT_MAX, holdScore = FLT_MAX;
	for (int i = 0; i < ff::Census_Count(); ++i)
	{
		const ff::CensusObj *o = ff::Census_At(i);
		if (o->kind != ff::OBJ_CPZONE || o->disabled)
			continue;
		CBaseEntity *zone = PF2_EntityByIndex(o->ent);
		if (!zone)
			continue;
		Vector c = zone->WorldSpaceCenter();
		if (!OwnedBySide(o, team))
		{
			// spread out a little: prefer the nearest, with some per-bot jitter
			float d = (c - from).Length() * RandomFloat(0.8f, 1.2f);
			if (d < takeDist)
			{
				takeDist = d;
				bestTake = o->ent;
			}
		}
		else
		{
			// hold the owned zone closest to an enemy-owned/neutral one (the front line)
			float front = FLT_MAX;
			for (int j = 0; j < ff::Census_Count(); ++j)
			{
				const ff::CensusObj *p = ff::Census_At(j);
				CBaseEntity *pz = p->kind == ff::OBJ_CPZONE && !OwnedBySide(p, team) ? PF2_EntityByIndex(p->ent) : NULL;
				if (pz)
					front = MIN(front, (pz->WorldSpaceCenter() - c).Length());
			}
			float score = front * RandomFloat(0.8f, 1.2f);
			if (score < holdScore)
			{
				holdScore = score;
				bestHold = o->ent;
			}
		}
	}
	return bestTake ? bestTake : bestHold;
}

ActionResult<CTFBot> CTFBotFFCommandPoint::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_zone = 0;
	m_rethinkTimer.Invalidate();
	m_repathTimer.Invalidate();
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	return Continue();
}

ActionResult<CTFBot> CTFBotFFCommandPoint::Update(CTFBot *me, float interval)
{
	if (!IsPossible(me))
		return Done("No command points");

	if (!m_zone || m_rethinkTimer.IsElapsed())
	{
		m_rethinkTimer.Start(RandomFloat(3.0f, 6.0f));
		int zone = ChooseZone(me);
		if (zone != m_zone)
		{
			m_zone = zone;
			m_repathTimer.Invalidate();
			CBaseEntity *z = PF2_EntityByIndex(m_zone);
			if (z)
			{
				// a reachable spot inside the zone
				m_spot = z->WorldSpaceCenter();
				CNavArea *a = TheNavMesh->GetNearestNavArea(m_spot, false, 400.0f, false, true);
				if (a)
					m_spot = a->GetCenter();
			}
		}
	}
	CBaseEntity *zone = PF2_EntityByIndex(m_zone);
	if (!zone)
		return Continue();

	// in the zone: hold position (the main action fights; standing in it captures / defends)
	Extent ext;
	ext.Init(zone);
	const Vector &feet = me->GetAbsOrigin();
	bool inside = feet.x >= ext.lo.x && feet.x <= ext.hi.x && feet.y >= ext.lo.y && feet.y <= ext.hi.y &&
		feet.z >= ext.lo.z - 32.0f && feet.z <= ext.hi.z;
	if (inside && me->IsRangeLessThan(m_spot, 150.0f))
		return Continue();

	if (m_repathTimer.IsElapsed())
	{
		m_repathTimer.Start(RandomFloat(1.0f, 2.0f));
		CTFBotPathCost cost(me, FASTEST_ROUTE);
		m_path.Compute(me, m_spot, cost);
	}
	m_path.Update(me);
	return Continue();
}

ActionResult<CTFBot> CTFBotFFCommandPoint::OnResume(CTFBot *me, Action<CTFBot> *interruptingAction)
{
	m_repathTimer.Invalidate();
	m_rethinkTimer.Invalidate();
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotFFCommandPoint::OnStuck(CTFBot *me)
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

EventDesiredResult<CTFBot> CTFBotFFCommandPoint::OnMoveToFailure(CTFBot *me, const Path *path, MoveToFailureType reason)
{
	m_repathTimer.Invalidate();
	m_rethinkTimer.Invalidate();   // maybe another zone
	return TryContinue();
}
