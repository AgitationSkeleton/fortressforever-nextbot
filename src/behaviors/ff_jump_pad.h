// ff-nextbot: scouts build a jump pad (FF "mancannon") and ride it toward their objective.
//
// A mancannon is built 80 units in front of the scout along its view direction and launches whoever
// touches it with velocity forward * 768 + up * 512 (CFFManCannon::OnObjectTouch), roughly a
// 1000-unit hop. Before building, the scout simulates that arc with player-hull traces for a few
// directions around the way to its goal and only builds when the arc lands on a floor that is on the
// mesh, not blocked for its team, and at least ff_bot_jumppad_min_gain units closer (by path) to the
// goal than the pad itself. Then it waits for the pad to finish building and runs onto it.
#pragma once
#include "bot/tf_bot.h"

class CTFBotFFJumpPad : public Action<CTFBot>
{
public:
	CTFBotFFJumpPad(const Vector &padSpot, float yaw) : m_padSpot(padSpot), m_yaw(yaw) {}

	// a launch toward goal that pays off? fills the pad spot and yaw to build with
	static bool FindLaunch(CTFBot *me, const Vector &goal, Vector *padSpot, float *yaw);

	virtual ActionResult<CTFBot> OnStart(CTFBot *me, Action<CTFBot> *priorAction);
	virtual ActionResult<CTFBot> Update(CTFBot *me, float interval);
	virtual QueryResultType ShouldRetreat(const INextBot *me) const { return ANSWER_NO; }
	virtual const char *GetName(void) const { return "FFJumpPad"; }

private:
	Vector m_padSpot;
	float m_yaw;
	bool m_ordered = false;
	CountdownTimer m_giveUpTimer;
	CountdownTimer m_orderTimer;
};
