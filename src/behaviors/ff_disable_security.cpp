// ff-nextbot: turn off an enemy team's security (see ff_disable_security.h)
#include "cbase.h"
#include "ff_disable_security.h"
#include "game/ff_census.h"
#include "nav_mesh.h"
#include "tf_nav_mesh.h"

static int NearestSwitch(CTFBot *me, int team)
{
	int best = 0;
	float bestDist = FLT_MAX;
	for (int i = 0; i < ff::Census_Count(); ++i)
	{
		const ff::CensusObj *o = ff::Census_At(i);
		if (o->kind != ff::OBJ_SECURITY || o->owner != team || o->disabled)
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

bool CTFBotFFDisableSecurity::IsNeeded(CTFBot *me, int team)
{
	return ff::Census_SecurityUp(team) && NearestSwitch(me, team) != 0;
}

ActionResult<CTFBot> CTFBotFFDisableSecurity::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_switch = NearestSwitch(me, m_team);
	if (!m_switch)
		return Done("No security switch");
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	m_repathTimer.Invalidate();
	m_giveUpTimer.Start(45.0f);
	m_pathFailures = 0;
	return Continue();
}

ActionResult<CTFBot> CTFBotFFDisableSecurity::Update(CTFBot *me, float interval)
{
	if (!ff::Census_SecurityUp(m_team))
		return Done("Security is down");
	if (m_giveUpTimer.IsElapsed())
		return Done("Took too long to reach the security switch");
	CBaseEntity *sw = PF2_EntityByIndex(m_switch);
	if (!sw)
		return Done("Security switch is gone");

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if (threat)
		me->EquipBestWeaponForThreat(threat);

	Vector goal = sw->WorldSpaceCenter();
	Vector mins, maxs;
	sw->CollisionProp()->WorldSpaceAABB(&mins, &maxs);
	Vector feet = me->GetAbsOrigin();
	bool inside = feet.x >= mins.x - 24.0f && feet.x <= maxs.x + 24.0f && feet.y >= mins.y - 24.0f && feet.y <= maxs.y + 24.0f &&
		feet.z + 72.0f >= mins.z && feet.z <= maxs.z + 24.0f;
	bool close = (goal - me->EyePosition()).IsLengthLessThan(140.0f);

	if (close || inside)
	{
		// a button: look at it, press use and hit it; a trigger: walk into its middle
		if (!threat || !threat->IsVisibleRecently())
		{
			me->GetBodyInterface()->AimHeadTowards(goal, IBody::CRITICAL, 0.3f, NULL, "Pressing the security switch");
			if (me->GetBodyInterface()->IsHeadAimingOnTarget())
			{
				me->PressUseButton(0.1f);
				me->PressFireButton(0.1f);
			}
		}
		Vector to = goal - feet;
		to.z = 0.0f;
		if (to.IsLengthGreaterThan(8.0f))
			me->GetLocomotionInterface()->Approach(goal);
		return Continue();
	}

	if (m_repathTimer.IsElapsed())
	{
		m_repathTimer.Start(RandomFloat(1.0f, 2.0f));
		CTFBotPathCost cost(me, FASTEST_ROUTE);
		if (!m_path.Compute(me, goal, cost))
		{
			if (++m_pathFailures >= 3)
				return Done("Security switch unreachable");
		}
		else
			m_pathFailures = 0;
	}
	m_path.Update(me);
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotFFDisableSecurity::OnStuck(CTFBot *me)
{
	m_repathTimer.Invalidate();
	me->GetLocomotionInterface()->Jump();
	return TryContinue();
}
