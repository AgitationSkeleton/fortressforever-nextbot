// ff-nextbot: turn off an enemy team's security (base_shutdown maps: ff_destroy, ff_schtop, ...).
//
// On these maps a team's flag room sits behind lasers, a security wall or a door that only opens
// while that team's security is down. An enemy turns it off by touching the team's security trigger
// (<team>_security_trigger and map subclasses) or pressing its button (button_<team>), after which
// it stays down for SECURITY_LENGTH seconds. The census reports the switches and the state
// (button_<team>.sec_up). A bot fetching a flag whose team's security is up runs this first: it goes
// to the nearest switch, touches it, and also presses use and fires at it (FF buttons can be
// touch, use or damage activated), until the state reads down.
#pragma once
#include "bot/tf_bot.h"
#include "Path/NextBotPathFollow.h"

class CTFBotFFDisableSecurity : public Action<CTFBot>
{
public:
	CTFBotFFDisableSecurity(int team) : m_team(team) {}
	static bool IsNeeded(CTFBot *me, int team);   // team's security is up and the map has a switch for it

	virtual ActionResult<CTFBot> OnStart(CTFBot *me, Action<CTFBot> *priorAction);
	virtual ActionResult<CTFBot> Update(CTFBot *me, float interval);
	virtual EventDesiredResult<CTFBot> OnStuck(CTFBot *me);
	virtual QueryResultType ShouldHurry(const INextBot *me) const { return ANSWER_YES; }
	virtual QueryResultType ShouldRetreat(const INextBot *me) const { return ANSWER_NO; }
	virtual const char *GetName(void) const { return "FFDisableSecurity"; }

private:
	int m_team;        // whose security
	int m_switch = 0;  // entity index of the switch we are going to
	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_giveUpTimer;
	int m_pathFailures = 0;
};
