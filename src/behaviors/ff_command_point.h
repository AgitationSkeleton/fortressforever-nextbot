// ff-nextbot: Fortress Forever command points (base_cp: ff_cz2, ff_warpath ...).
//
// A command point is a trigger zone a team captures by standing in it; its owner (the "defending
// team") lives in Lua and reaches us through the census (ffnb_owner events). Bots go for the
// nearest zone their side does not own and stand in it; when their side owns every zone they hold
// one, preferring the zones nearest the enemy. Fighting is done by the main action as usual.
#pragma once
#include "bot/tf_bot.h"
#include "Path/NextBotPathFollow.h"

class CTFBotFFCommandPoint : public Action<CTFBot>
{
public:
	static bool IsPossible(CTFBot *me);   // the map has command point zones

	virtual ActionResult<CTFBot> OnStart(CTFBot *me, Action<CTFBot> *priorAction);
	virtual ActionResult<CTFBot> Update(CTFBot *me, float interval);
	virtual ActionResult<CTFBot> OnResume(CTFBot *me, Action<CTFBot> *interruptingAction);
	virtual EventDesiredResult<CTFBot> OnStuck(CTFBot *me);
	virtual EventDesiredResult<CTFBot> OnMoveToFailure(CTFBot *me, const Path *path, MoveToFailureType reason);
	virtual const char *GetName(void) const { return "FFCommandPoint"; }

private:
	int ChooseZone(CTFBot *me);   // entity index of the zone to go to, 0 if none
	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_rethinkTimer;
	int m_zone = 0;
	Vector m_spot;
};
