// ff-nextbot: demomen plant detpacks on the map's detpack targets.
//
// FF maps mark what a detpack can blow up with Lua triggers that handle explosions themselves
// (base_teamplay's <team>_detpack_trigger opening a wall through <team>_det_relay, base_cp's command
// centres, map-specific ones such as ff_cornfield's wall). The census flags them (detTarget). A
// demoman carrying a detpack picks the nearest target that belongs to another team (or nobody),
// walks into it, sets a 5 s detpack ("detpack 5") and then gets away from it.
#pragma once
#include "bot/tf_bot.h"
#include "Path/NextBotPathFollow.h"

class CTFBotFFPlantDetpack : public Action<CTFBot>
{
public:
	static bool IsPossible(CTFBot *me);

	virtual ActionResult<CTFBot> OnStart(CTFBot *me, Action<CTFBot> *priorAction);
	virtual ActionResult<CTFBot> Update(CTFBot *me, float interval);
	virtual EventDesiredResult<CTFBot> OnStuck(CTFBot *me);
	virtual QueryResultType ShouldRetreat(const INextBot *me) const { return ANSWER_NO; }
	virtual const char *GetName(void) const { return "FFPlantDetpack"; }

private:
	int m_target = 0;
	bool m_planted = false;
	Vector m_detpackSpot;
	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_giveUpTimer;
	CountdownTimer m_fleeTimer;
	int m_pathFailures = 0;
};
