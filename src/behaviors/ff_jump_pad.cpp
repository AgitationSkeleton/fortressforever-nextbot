// ff-nextbot: scouts build and ride jump pads (see ff_jump_pad.h)
#include "cbase.h"
#include "ff_jump_pad.h"
#include "nav_mesh.h"
#include "nav_pathfind.h"
#include "tf_nav_mesh.h"

ConVar ff_bot_jumppad("ff_bot_jumppad", "1", FCVAR_NONE, "Scouts build jump pads (mancannons) where a simulated launch lands them well along their route");
ConVar ff_bot_jumppad_min_gain("ff_bot_jumppad_min_gain", "600", FCVAR_NONE, "A jump pad launch must cut at least this much path distance to the goal");

static const int FF_AMMO_MANCANNON = 6;          // FF GetAmmoDef order (see shim/pf2_tf.cpp)
static const float PAD_BUILD_DIST = 80.0f;       // FF_BUILD_MC_BUILD_DIST
static const float PAD_PUSH_FORWARD = 768.0f;    // ffdev_mancannon_push_forward default
static const float PAD_PUSH_UP = 512.0f;         // MANCANNON_PUSH_UP

extern void PF2NB_Log(const char *fmt, ...);

static CBaseEntity *MyPad(CTFBot *me)
{
	return me->HandleField("m_hManCannon");
}

// simulate the launch from the pad; returns the landing spot (feet) or false if the arc hits a wall,
// leaves the map or never lands within 3 s
static bool SimulateLaunch(CTFBot *me, const Vector &pad, const Vector &fwd, Vector *landing)
{
	static ConVarRef sv_gravity("sv_gravity");
	float g = sv_gravity.IsValid() ? sv_gravity.GetFloat() : 800.0f;
	Vector pos = pad + Vector(0, 0, 18.0f);   // standing on the pad
	Vector vel = fwd * PAD_PUSH_FORWARD + Vector(0, 0, PAD_PUSH_UP);
	const Vector mins(-16, -16, 0), maxs(16, 16, 72);
	const float dt = 0.05f;
	for (float t = 0.0f; t < 3.0f; t += dt)
	{
		Vector next = pos + vel * dt;
		vel.z -= g * dt;
		trace_t tr;
		UTIL_TraceHull(pos, next, mins, maxs, MASK_PLAYERSOLID, me, COLLISION_GROUP_PLAYER_MOVEMENT, &tr);
		if (tr.startsolid)
			return false;
		if (tr.fraction < 1.0f)
		{
			if (tr.plane.normal.z < 0.7f)
				return false;   // hit a wall or a slope too steep to land on
			*landing = tr.endpos;
			return true;
		}
		pos = next;
	}
	return false;
}

bool CTFBotFFJumpPad::FindLaunch(CTFBot *me, const Vector &goal, Vector *padSpot, float *yaw)
{
	if (!ff_bot_jumppad.GetBool() || !me->IsPlayerClass(TF_CLASS_SCOUT))
		return false;
	if (me->CBaseCombatCharacter::GetAmmoCount(FF_AMMO_MANCANNON) <= 0 || MyPad(me))
		return false;
	if (!me->GetLocomotionInterface()->IsOnGround())
		return false;
	CNavArea *goalArea = TheNavMesh->GetNearestNavArea(goal, true, 500.0f);
	if (!goalArea)
		return false;
	ShortestPathCost cost;
	Vector to = goal - me->GetAbsOrigin();
	to.z = 0.0f;
	if (to.IsLengthLessThan(1200.0f))
		return false;   // close enough to just run
	float baseYaw = RAD2DEG(atan2f(to.y, to.x));
	static const float offsets[] = { 0.0f, -20.0f, 20.0f, -40.0f, 40.0f };
	float bestGain = ff_bot_jumppad_min_gain.GetFloat();
	bool found = false;
	for (float off : offsets)
	{
		float y = baseYaw + off;
		Vector fwd(cosf(DEG2RAD(y)), sinf(DEG2RAD(y)), 0.0f);
		Vector pad = me->GetAbsOrigin() + fwd * PAD_BUILD_DIST;
		// the pad needs floor under it, about level with us
		trace_t tr;
		UTIL_TraceLine(pad + Vector(0, 0, 32), pad - Vector(0, 0, 48), MASK_PLAYERSOLID, me, COLLISION_GROUP_NONE, &tr);
		if (tr.startsolid || tr.fraction >= 1.0f || tr.plane.normal.z < 0.9f)
			continue;
		pad = tr.endpos;
		CNavArea *padArea = TheNavMesh->GetNavArea(pad, 64.0f);
		Vector landing;
		if (!padArea || !SimulateLaunch(me, pad, fwd, &landing))
			continue;
		CNavArea *landArea = TheNavMesh->GetNavArea(landing + Vector(0, 0, 8), 64.0f);
		if (!landArea || landArea->IsBlocked(me->GetTeamNumber()))
			continue;
		float before = NavAreaTravelDistance(padArea, goalArea, cost);
		float after = NavAreaTravelDistance(landArea, goalArea, cost);
		if (before < 0.0f || after < 0.0f)
			continue;
		float gain = before - after;
		if (gain > bestGain)
		{
			bestGain = gain;
			*padSpot = pad;
			*yaw = y;
			found = true;
		}
	}
	return found;
}

ActionResult<CTFBot> CTFBotFFJumpPad::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_ordered = false;
	m_giveUpTimer.Start(15.0f);
	m_orderTimer.Start(0.6f);   // turn to face the launch direction first
	return Continue();
}

ActionResult<CTFBot> CTFBotFFJumpPad::Update(CTFBot *me, float interval)
{
	if (m_giveUpTimer.IsElapsed())
		return Done("Jump pad took too long");
	Vector fwd(cosf(DEG2RAD(m_yaw)), sinf(DEG2RAD(m_yaw)), 0.0f);
	CBaseEntity *pad = MyPad(me);
	if (!m_ordered)
	{
		me->GetLocomotionInterface()->Stop();
		// look level along the launch direction: FF builds the pad 80 units ahead along the view
		me->GetBodyInterface()->AimHeadTowards(me->EyePosition() + fwd * 500.0f, IBody::CRITICAL, 0.5f, NULL, "Placing a jump pad");
		if (m_orderTimer.IsElapsed() && me->GetBodyInterface()->IsHeadAimingOnTarget())
		{
			me->ClientCommand("mancannon");
			m_ordered = true;
			PF2NB_Log("%s builds a jump pad at (%.0f %.0f %.0f) yaw %.0f", me->GetPlayerName(), m_padSpot.x, m_padSpot.y, m_padSpot.z, m_yaw);
		}
		return Continue();
	}
	if (!pad)
	{
		me->GetLocomotionInterface()->Stop();
		return Continue();   // still building
	}
	bool *built = pad->Field<bool>("m_bBuilt");
	if (built && !*built)
	{
		me->GetLocomotionInterface()->Stop();
		return Continue();
	}
	// launched? (the pad sets our velocity; we leave the ground going up fast)
	if (!me->GetLocomotionInterface()->IsOnGround() && me->GetAbsVelocity().z > 300.0f)
	{
		PF2NB_Log("%s launched off its jump pad at (%.0f %.0f %.0f)", me->GetPlayerName(), me->GetAbsOrigin().x, me->GetAbsOrigin().y, me->GetAbsOrigin().z);
		return Done("Launched");
	}
	// the pad launches whoever touches it while holding jump, along that player's view yaw
	// (CFFManCannon::OnObjectTouch), so face the planned direction, walk onto it and hold jump
	me->GetBodyInterface()->AimHeadTowards(me->EyePosition() + fwd * 500.0f, IBody::CRITICAL, 0.2f, NULL, "Riding the jump pad");
	me->GetLocomotionInterface()->Approach(pad->GetAbsOrigin());
	Vector d = pad->GetAbsOrigin() - me->GetAbsOrigin();
	d.z = 0.0f;
	if (d.IsLengthLessThan(90.0f))
		me->PressJumpButton(0.3f);
	return Continue();
}
