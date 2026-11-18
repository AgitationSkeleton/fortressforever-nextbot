// ff-nextbot: Fortress Forever class skills that ride on the class-skill button (IN_ATTACK2).
//
// FF maps IN_ATTACK2 to a per-class skill (CFFPlayer::ClassSpecificSkill*), not to TF2's alt-fire:
//   pyro    - jetpack while held in the air (fuel, recharges on the ground)
//   medic   - throw a health pack (costs 10 cells)
//   demoman - detonate pipes (TFBot's sticky logic already does the right thing)
//   hwguy   - overpressure (knock-back blast)
// TFBot presses alt-fire for TF2 reasons (pyro airblast, medic uber), so for pyro and medic the
// button is taken over here: pyros jetpack up ledges they jump at, when stuck and after enemies
// above them; medics never drop packs by accident.
#include "cbase.h"
#include "bot/tf_bot.h"
#include "NextBotVisionInterface.h"
#include "in_buttons.h"

ConVar ff_bot_jetpack("ff_bot_jetpack", "1", FCVAR_NONE, "Pyros use the jetpack to climb, get unstuck and chase enemies above them");

struct AbilityState
{
	CTFBot *bot = NULL;
	float jetUntil = 0.0f;     // hold the jetpack until then (while airborne)
	float nextJet = 0.0f;      // earliest next jetpack burst
	bool wantJump = false;     // jump first, then jetpack once off the ground
	float detonateDispenserAt = 0.0f;   // spy: blow up a sabotaged dispenser then (0 = none pending)
};
static AbilityState s_ab[MAX_PLAYERS + 1];

void FF_ScheduleDispenserDetonate(CTFBot *bot, float delay)
{
	int idx = bot ? bot->entindex() : 0;
	if (idx >= 1 && idx <= MAX_PLAYERS && s_ab[idx].bot == bot && s_ab[idx].detonateDispenserAt == 0.0f)
		s_ab[idx].detonateDispenserAt = gpGlobals->curtime + delay;
}

static void PyroJetpack(CTFBot *bot, CUserCmd *cmd, AbilityState &s)
{
	cmd->buttons &= ~IN_ATTACK2;   // no airblast in FF; the button is the jetpack
	if (!ff_bot_jetpack.GetBool())
		return;
	const float now = gpGlobals->curtime;
	ILocomotion *loc = bot->GetLocomotionInterface();
	bool onGround = loc->IsOnGround();

	if (now >= s.nextJet && now >= s.jetUntil)
	{
		bool start = false;
		// 1) TFBot is jumping up a ledge or gap: a burst makes it
		if (loc->IsClimbingOrJumping() && !onGround)
			start = true;
		// 2) stuck: jump and fly over whatever is in the way
		else if (loc->IsStuck() && loc->GetStuckDuration() > 1.0f)
			start = true;
		// 3) a visible enemy well above and close: go up after it
		else if (const CKnownEntity *threat = bot->GetVisionInterface()->GetPrimaryKnownThreat(true))
		{
			CBaseEntity *e = threat->GetEntity();
			if (e && threat->IsVisibleRecently())
			{
				Vector d = e->GetAbsOrigin() - bot->GetAbsOrigin();
				if (d.z > 96.0f && d.Length2D() < 600.0f && RandomFloat(0.0f, 1.0f) < 0.05f)
					start = true;
			}
		}
		if (start)
		{
			s.jetUntil = now + RandomFloat(0.4f, 0.9f);
			s.nextJet = s.jetUntil + 2.0f;
			s.wantJump = onGround;
		}
	}

	if (now < s.jetUntil)
	{
		if (onGround && s.wantJump)
		{
			cmd->buttons |= IN_JUMP;   // the jetpack only works in the air
			s.wantJump = false;
		}
		else if (!onGround)
			cmd->buttons |= IN_ATTACK2;
	}
}

void FF_Abilities_Think(CTFBot *bot, CUserCmd *cmd)
{
	int idx = bot->entindex();
	if (idx < 1 || idx > MAX_PLAYERS)
		return;
	AbilityState &s = s_ab[idx];
	if (s.bot != bot)
	{
		s = AbilityState();
		s.bot = bot;
	}
	switch (bot->GetPlayerClass()->GetClassIndex())
	{
	case TF_CLASS_PYRO:
		PyroJetpack(bot, cmd, s);
		break;
	case TF_CLASS_MEDIC:
		cmd->buttons &= ~IN_ATTACK2;   // TFBot's uber button would throw away health packs and cells
		break;
	case TF_CLASS_SPY:
		if (s.detonateDispenserAt > 0.0f && gpGlobals->curtime >= s.detonateDispenserAt)
		{
			s.detonateDispenserAt = 0.0f;
			bot->ClientCommand("dispensersabotage");   // works dead or alive (FF_CMD_DEAD)
		}
		break;
	default:
		break;
	}
}
