// ff-nextbot: Hunted assassins (teams hostile to the VIP's team) go after the VIP: path to the VIP
// while it lives, otherwise wait near the escape. Fighting is done by the main action.
#pragma once
#include "bot/tf_bot.h"
#include "Path/NextBotPathFollow.h"

class CTFBotFFHuntVIP : public Action<CTFBot>
{
public:
	virtual ActionResult<CTFBot> OnStart(CTFBot *me, Action<CTFBot> *priorAction);
	virtual ActionResult<CTFBot> Update(CTFBot *me, float interval);
	virtual EventDesiredResult<CTFBot> OnStuck(CTFBot *me);
	virtual const char *GetName(void) const { return "FFHuntVIP"; }

private:
	PathFollower m_path;
	CountdownTimer m_repathTimer;
};
