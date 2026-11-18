// pf2-nextbot: defending team on PF2 flag Attack/Defend maps. See pf2_bot_defend_flag.h.
#include "cbase.h"
#include "pf2_bot_defend_flag.h"
#include "tf_nav_mesh.h"
#include "nav_mesh.h"

ConVar tf_bot_pf2_flag_guard_radius("ff_bot_flag_guard_radius", "500", FCVAR_NONE, "How far from the objective PF2 A/D defenders take up guard positions");

// TF2 semantics (CTFBot::GetFlagToFetch): an Attack/Defend flag is moved by the team that owns it
// (or anyone, if neutral). The other team defends it.
// FFNB: Fortress Forever: defend a flag that a hostile team can capture and we cannot (our own CTF
// flag, or the attackers' flag on attack/defend and invade/defend maps)
extern bool FF_TeamCanCaptureFlag(int team, const CCaptureFlag *flag);
extern bool FF_AreTeamsFriendly(int a, int b);
CCaptureFlag *CTFBotPF2DefendFlag::FlagToDefend(CTFBot *me)
{
	int mine = me->GetTeamNumber();
	for (int i = 0; i < ICaptureFlagAutoList::AutoList().Count(); ++i)
	{
		CCaptureFlag *flag = static_cast<CCaptureFlag *>(ICaptureFlagAutoList::AutoList()[i]);
		if (flag->IsDisabled() || FF_TeamCanCaptureFlag(mine, flag))
			continue;
		for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
			if (!FF_AreTeamsFriendly(mine, t) && FF_TeamCanCaptureFlag(t, flag))
				return flag;
	}
	return NULL;
}

ConVar ff_bot_ctf_defense_ratio("ff_bot_ctf_defense_ratio", "0.3", FCVAR_NONE, "Share of a team's bots that defend its flag when the team also has a flag to fetch (CTF)");

// FFNB: defender role by slot order among this team's bots (stable, no extra state). With nothing to
// fetch (attack/defend defenders) every bot defends.
bool FF_BotIsFlagDefender(CTFBot *me)
{
	if (!me->GetFlagToFetch())
		return true;
	int team = me->GetTeamNumber(), rank = 0, bots = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (!p || !p->IsBot() || p->GetTeamNumber() != team)
			continue;
		if (p->IsPlayerClass(TF_CLASS_ENGINEER) || p->IsPlayerClass(TF_CLASS_SNIPER))
			continue;   // they have their own jobs
		if (i < me->entindex())
			++rank;
		++bots;
	}
	int defenders = (int)(bots * ff_bot_ctf_defense_ratio.GetFloat() + 0.5f);
	return rank < defenders;
}

// Guard where the attackers must go: their capture zone if the map has one, otherwise the flag.
Vector CTFBotPF2DefendFlag::ChooseGuardSpot(CTFBot *me, CCaptureFlag *flag)
{
	Vector objective = flag->GetAbsOrigin();
	float best = FLT_MAX;
	// FFNB: our own (CTF) flag: guard the flag itself. Someone else's flag (attack/defend): guard the
	// cap where the attackers deliver it.
	for (int i = 0; i < ICaptureZoneAutoList::AutoList().Count() && flag->GetTeamNumber() != me->GetTeamNumber(); ++i)
	{
		CCaptureZone *zone = static_cast<CCaptureZone *>(ICaptureZoneAutoList::AutoList()[i]);
		if (zone->IsDisabled() || !zone->Accepts(flag))
			continue;
		float d = (zone->WorldSpaceCenter() - flag->GetAbsOrigin()).LengthSqr();
		if (d < best)
		{
			best = d;
			objective = zone->WorldSpaceCenter();
		}
	}

	// a random reachable nav area near the objective
	CNavArea *centre = TheNavMesh->GetNearestNavArea(objective, false, 500.0f, false, true);
	if (!centre)
		return objective;
	CUtlVector<CNavArea *> nearby;
	CollectSurroundingAreas(&nearby, centre, tf_bot_pf2_flag_guard_radius.GetFloat(), StepHeight, StepHeight);
	for (int tries = 0; tries < 8 && nearby.Count(); ++tries)
	{
		CNavArea *a = nearby[RandomInt(0, nearby.Count() - 1)];
		if (me->GetLocomotionInterface()->IsAreaTraversable(a))
			return a->GetCenter();
	}
	return centre->GetCenter();
}

ActionResult<CTFBot> CTFBotPF2DefendFlag::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	m_haveGuardSpot = false;
	m_repathTimer.Invalidate();
	m_guardTimer.Invalidate();
	return Continue();
}

ActionResult<CTFBot> CTFBotPF2DefendFlag::OnResume(CTFBot *me, Action<CTFBot> *interruptingAction)
{
	m_repathTimer.Invalidate();
	return Continue();
}

ActionResult<CTFBot> CTFBotPF2DefendFlag::Update(CTFBot *me, float interval)
{
	CCaptureFlag *flag = FlagToDefend(me);
	if (!flag)
		return Done("No attack/defend flag to defend");

	Vector goal;
	if (flag->IsStolen())
	{
		// run down the carrier; TFBot's MainAction fires at visible threats on its own
		CBaseEntity *carrier = flag->GetCarrier();
		goal = carrier ? carrier->GetAbsOrigin() : flag->GetAbsOrigin();
		m_haveGuardSpot = false;
	}
	else if (flag->IsDropped())
	{
		goal = flag->GetAbsOrigin();   // deny the dropped flag until it returns
		m_haveGuardSpot = false;
	}
	else
	{
		if (!m_haveGuardSpot || m_guardTimer.IsElapsed())
		{
			m_guardSpot = ChooseGuardSpot(me, flag);
			m_haveGuardSpot = true;
			m_guardTimer.Start(RandomFloat(15.0f, 30.0f));
			m_repathTimer.Invalidate();
		}
		goal = m_guardSpot;
		if (me->IsRangeLessThan(goal, 50.0f))
		{
			// on station: watch the approaches instead of walking in circles
			me->UpdateLookingAroundForIncomingPlayers(true);
			return Continue();
		}
	}

	if (m_repathTimer.IsElapsed())
	{
		m_repathTimer.Start(flag->IsStolen() ? 0.5f : 2.0f);
		CTFBotPathCost cost(me, FASTEST_ROUTE);
		m_path.Compute(me, goal, cost);
	}
	m_path.Update(me);
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotPF2DefendFlag::OnStuck(CTFBot *me)
{
	m_haveGuardSpot = false;
	m_repathTimer.Invalidate();
	return TryContinue();
}

QueryResultType CTFBotPF2DefendFlag::ShouldHurry(const INextBot *me) const
{
	return ANSWER_UNDEFINED;
}
