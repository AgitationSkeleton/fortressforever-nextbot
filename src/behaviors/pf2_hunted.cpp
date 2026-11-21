// pf2-nextbot: PF2 hunted/VIP maps and per-map ladders. See pf2_hunted.h.
#include "cbase.h"
#include "pf2_hunted.h"
#include "bot/behavior/sniper/tf_bot_sniper_attack.h"
#include "game/ff_census.h"
#include "ff_hunt_vip.h"
#include "nav_mesh.h"
#include "nav_ladder.h"
#include "tf_nav_mesh.h"
#include "bot/behavior/tf_bot_escort.h"
#include "bot/behavior/tf_bot_seek_and_destroy.h"

ConVar tf_bot_pf2_hunted_rules("ff_bot_hunted_rules", "1", FCVAR_NONE, "Apply addons/ffnextbot/data/hunted.cfg (class limits, VIP objectives) on hunted maps");
ConVar tf_bot_pf2_hunted_bot_vip("ff_bot_hunted_bot_vip", "1", FCVAR_NONE, "On hunted maps, a BLU bot plays the Civilian VIP when no one else does");
ConVar tf_bot_pf2_ladders("ff_bot_ladders", "1", FCVAR_NONE, "Add nav ladders from addons/ffnextbot/data/ladders.cfg (PF2 info_ladder volumes) at map load");

static PF2HuntedRules s_rules;
const PF2HuntedRules &PF2Hunted_Rules() { return s_rules; }

// FFNB: the VIP's team (FF base_hunted: Blue, Civilian only); bodyguards are teams allied to it
static int s_vipTeam = TF_TEAM_BLUE;
extern bool FF_AreTeamsFriendly(int a, int b);
extern int FF_TeamClassLimit(int team, int tfclass);

// FFNB: Fortress Forever Hunted maps are described by the Lua census: the escape trigger is the VIP's
// goal and the VIP team is the one whose Lua class limits allow only the Civilian.
void PF2Hunted_FromCensus()
{
	const ff::CensusObj *escape = NULL;
	for (int i = 0; i < ff::Census_Count(); ++i)
		if (ff::Census_At(i)->kind == ff::OBJ_HUNTED_ESCAPE)
		{
			escape = ff::Census_At(i);
			break;
		}
	CBaseEntity *e = escape ? PF2_EntityByIndex(escape->ent) : NULL;
	if (!e || !tf_bot_pf2_hunted_rules.GetBool())
		return;
	s_vipTeam = TF_TEAM_BLUE;
	for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
	{
		bool onlyCiv = FF_TeamClassLimit(t, TF_CLASS_CIVILIAN) != -1;
		for (int c = TF_CLASS_SCOUT; c <= TF_CLASS_ENGINEER && onlyCiv; ++c)
			if (FF_TeamClassLimit(t, c) != -1)
				onlyCiv = false;
		if (onlyCiv)
		{
			s_vipTeam = t;
			break;
		}
	}
	s_rules.active = true;
	s_rules.hasGoal = true;
	Extent ext;
	ext.Init(e);
	s_rules.goalMins = ext.lo;
	s_rules.goalMaxs = ext.hi;

	// the escape door: the func_door nearest the escape zone; its buttons are the use/touch buttons near
	// that door outside the zone (ff_hunted: escape_door_top/bottom, escape_door_button_left/right)
	Vector zoneCentre = (ext.lo + ext.hi) * 0.5f;
	CBaseEntity *door = NULL;
	float best = 500.0f * 500.0f;
	for (CBaseEntity *d = gEntList.FindEntityByClassname(NULL, "func_door*"); d; d = gEntList.FindEntityByClassname(d, "func_door*"))
	{
		float dist = (d->WorldSpaceCenter() - zoneCentre).LengthSqr();
		if (dist < best && STRING(d->GetEntityName())[0])
		{
			best = dist;
			door = d;
		}
	}
	if (door)
	{
		V_strncpy(s_rules.gateDoor, STRING(door->GetEntityName()), sizeof(s_rules.gateDoor));
		Extent inside = ext;
		inside.lo -= Vector(32, 32, 32);
		inside.hi += Vector(32, 32, 32);
		static const char *buttonClasses[] = { "func_button", "func_rot_button" };
		for (const char *bc : buttonClasses)
			for (CBaseEntity *b = gEntList.FindEntityByClassname(NULL, bc); b; b = gEntList.FindEntityByClassname(b, bc))
			{
				Vector p = b->WorldSpaceCenter();
				if ((p - door->WorldSpaceCenter()).IsLengthGreaterThan(900.0f) || inside.Contains(p))
					continue;
				s_rules.buttons.AddToTail(p);
			}
	}
	Msg("[FFNB] hunted (census): gate door '%s', %d buttons\n", s_rules.gateDoor, s_rules.buttons.Count());
	Msg("[FFNB] hunted (census): VIP team %d, escape '%s' (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n", s_vipTeam, escape->name,
		ext.lo.x, ext.lo.y, ext.lo.z, ext.hi.x, ext.hi.y, ext.hi.z);
}

static const char *s_classKeys[] = { "scout", "sniper", "soldier", "demoman", "medic", "heavyweapons", "pyro", "spy", "engineer", "civilian" };

static bool ParseBox(const char *s, Vector *mins, Vector *maxs)
{
	float v[6];
	if (!s || sscanf(s, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return false;
	mins->Init(MIN(v[0], v[3]), MIN(v[1], v[4]), MIN(v[2], v[5]));
	maxs->Init(MAX(v[0], v[3]), MAX(v[1], v[4]), MAX(v[2], v[5]));
	return true;
}

static void LoadLadders(const char *mapname)
{
	if (!tf_bot_pf2_ladders.GetBool() || !TheNavMesh)
		return;
	KeyValues *kv = new KeyValues("PF2Ladders");
	int n = 0;
	if (kv->LoadFromFile(filesystem, "addons/ffnextbot/data/ladders.cfg", "MOD"))
	{
		if (KeyValues *map = kv->FindKey(mapname))
		{
			// ladders saved in the .nav (e.g. by pf2nb_nav_seed_ladders) are replaced by fresh ones, so each
			// connects to the areas that exist now (the generator saves them unconnected)
			TheNavMesh->PF2_DestroyLadders();
			for (KeyValues *l = map->GetFirstValue(); l; l = l->GetNextValue())
			{
				Vector mins, maxs;
				if (ParseBox(l->GetString(), &mins, &maxs))
				{
					TheNavMesh->CreateLadder(mins, maxs, 0.0f);
					++n;
				}
			}
		}
	}
	kv->deleteThis();
	if (n)
		Msg("[FFNB] %d ladders added to the nav mesh\n", n);
}

void PF2Hunted_LoadForMap(const char *mapname)
{
	s_rules = PF2HuntedRules();
	LoadLadders(mapname);
	if (!tf_bot_pf2_hunted_rules.GetBool())
		return;
	KeyValues *kv = new KeyValues("PF2HuntedRules");
	if (kv->LoadFromFile(filesystem, "addons/ffnextbot/data/hunted.cfg", "MOD"))
	{
		if (KeyValues *map = kv->FindKey(mapname))
		{
			s_rules.active = true;
			const char *teams[2] = { "red", "blue" };
			for (int t = 0; t < 2; ++t)
			{
				const char *list = map->GetString(teams[t], NULL);
				if (!list)
					continue;
				unsigned mask = 0;
				for (int c = 0; c < 10; ++c)
					if (V_stristr(list, s_classKeys[c]))
						mask |= 1u << c;
				s_rules.classMask[t == 0 ? TF_TEAM_RED : TF_TEAM_BLUE] = mask;
			}
			s_rules.hasGoal = ParseBox(map->GetString("vip_goal", NULL), &s_rules.goalMins, &s_rules.goalMaxs);
			s_rules.goalWait = map->GetBool("vip_wait", false);
			V_strncpy(s_rules.gateDoor, map->GetString("gate_door", ""), sizeof(s_rules.gateDoor));
			for (int i = 0; i < 16; ++i)
			{
				Vector p;
				const char *s = map->GetString(UTIL_VarArgs("button%d", i), NULL);
				if (s && sscanf(s, "%f %f %f", &p.x, &p.y, &p.z) == 3)
					s_rules.buttons.AddToTail(p);
			}
			Msg("[FFNB] hunted rules for %s: red=%x blue=%x goal=%d wait=%d gate=%s buttons=%d\n", mapname,
				s_rules.classMask[TF_TEAM_RED], s_rules.classMask[TF_TEAM_BLUE], (int)s_rules.hasGoal, (int)s_rules.goalWait,
				s_rules.gateDoor, s_rules.buttons.Count());
		}
	}
	kv->deleteThis();
	// FFNB: FF's startup() (and so the census) ran inside LevelInit, before this reset; re-apply
	if (!s_rules.active && ff::Census_Done())
		PF2Hunted_FromCensus();
}

bool PF2Hunted_ClassAllowed(int team, int tfclass, bool *hasRule)
{
	*hasRule = false;
	if (!s_rules.active || team < 0 || team >= TF_TEAM_COUNT || tfclass < 1 || tfclass > 10)
		return true;
	// the VIP (Civilian) belongs to the escorting team only - also after the map swaps teams
	if (tfclass == TF_CLASS_CIVILIAN && s_rules.hasGoal && team != s_vipTeam)
	{
		*hasRule = true;
		return false;
	}
	if (!s_rules.classMask[team])
		return true;
	*hasRule = true;
	return (s_rules.classMask[team] & (1u << (tfclass - 1))) != 0;
}

CTFPlayer *PF2Hunted_FindVIP(int team)
{
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (p && p->GetTeamNumber() == team && p->IsPlayerClass(TF_CLASS_CIVILIAN))
			return p;
	}
	return NULL;
}

// The VIP is BLU's Civilian. If BLU has none (and the map's rules allow one), the BLU bot with the
// lowest entity index takes the role when it next chooses a class.
bool PF2Hunted_WantsBotAsVIP(const CTFBot *bot)
{
	if (!s_rules.active || !s_rules.hasGoal || !tf_bot_pf2_hunted_bot_vip.GetBool() || bot->GetTeamNumber() != s_vipTeam)
		return false;
	bool rule;
	if (!PF2Hunted_ClassAllowed(s_vipTeam, TF_CLASS_CIVILIAN, &rule))
		return false;
	// only one VIP: anyone else already playing Civilian on BLU (human first) keeps the role
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (p && p != bot && p->GetTeamNumber() == s_vipTeam && p->IsPlayerClass(TF_CLASS_CIVILIAN))
			return false;
	}
	for (int i = 1; i < bot->entindex(); ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (p && p->IsBot() && p->GetTeamNumber() == s_vipTeam)
			return false;   // a lower-index BLU bot is the designated VIP
	}
	return true;
}

static CBaseEntity *FindGateDoor()
{
	if (!s_rules.gateDoor[0])
		return NULL;
	return gEntList.FindEntityByName(NULL, s_rules.gateDoor);
}

bool PF2Hunted_GateOpen()
{
	if (!s_rules.gateDoor[0])
		return true;
	bool sawDoor = false;
	for (CBaseEntity *d = gEntList.FindEntityByName(NULL, s_rules.gateDoor); d; d = gEntList.FindEntityByName(d, s_rules.gateDoor))
	{
		sawDoor = true;
		int *toggle = d->Field<int>("m_toggle_state");
		if (toggle && *toggle != TS_AT_BOTTOM)
			return true;   // opening or open
	}
	return !sawDoor;
}

static Vector GoalPoint()
{
	// where a player stands at the centre of the goal volume: the highest floor inside it (Stonehenge's
	// lift volume starts at the ground but the pillar top is 190 units up, so the bottom would be inside
	// the solid pillar and the path would end beside it)
	Vector p((s_rules.goalMins.x + s_rules.goalMaxs.x) * 0.5f, (s_rules.goalMins.y + s_rules.goalMaxs.y) * 0.5f, s_rules.goalMins.z);
	Vector top(p.x, p.y, s_rules.goalMaxs.z - 1.0f);
	float z;
	if (s_rules.goalWait && TheNavMesh && TheNavMesh->GetGroundHeight(top, &z) && z >= s_rules.goalMins.z - 64.0f && z <= s_rules.goalMaxs.z)
		p.z = z;
	return p;
}

Action<CTFBot> *PF2Hunted_ScenarioAction(CTFBot *me)
{
	// FFNB: the VIP plays on the VIP team; bodyguards are its team or any team allied to it;
	// every hostile team (FF's yellow assassins) hunts the VIP
	if (!s_rules.active || !s_rules.hasGoal)
		return NULL;
	if (!FF_AreTeamsFriendly(me->GetTeamNumber(), s_vipTeam))
		return me->IsPlayerClass(TF_CLASS_SNIPER) ? (Action<CTFBot> *)new CTFBotHuntedSniper : (Action<CTFBot> *)new CTFBotFFHuntVIP;   // snipers roam vantage points; the rest hunt the VIP
	if (me->IsPlayerClass(TF_CLASS_CIVILIAN) && me->GetTeamNumber() == s_vipTeam)
		return new CTFBotPF2HuntedVIP;
	if (me->IsPlayerClass(TF_CLASS_MEDIC))
		return NULL;   // medics keep TFBot's own heal behavior (they follow and heal the VIP's escort)
	return new CTFBotPF2HuntedBodyguard;
}

static int HuntedVipTeam() { return s_vipTeam; }

// ---------------------------------------------------------------------------------------------
// Route variety: scale the path cost by a per-life random factor for each 600-unit region, so
// successive VIPs prefer different corridors to the goal (Stonehenge always went the same way)
// ---------------------------------------------------------------------------------------------
class HuntedRouteCost : public IPathCost
{
public:
	HuntedRouteCost(CTFBot *me, unsigned seed) : m_base(me, SAFEST_ROUTE), m_seed(seed) {}
	virtual float operator()(CNavArea *area, CNavArea *fromArea, const CNavLadder *ladder, const CFuncElevator *elevator, float length) const
	{
		float c = m_base(area, fromArea, ladder, elevator, length);
		if (c < 0.0f || !fromArea)
			return c;
		float prev = fromArea->GetCostSoFar();
		const Vector &p = area->GetCenter();
		unsigned h = (unsigned)(int)floorf(p.x / 600.0f) * 73856093u ^ (unsigned)(int)floorf(p.y / 600.0f) * 19349663u ^ m_seed;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		float k = 1.0f + 2.0f * (float)(h & 0xffff) / 65535.0f;   // 1 .. 3
		return prev + (c - prev) * k;
	}
private:
	CTFBotPathCost m_base;
	unsigned m_seed;
};

// the VIP's nearest living teammate (bodyguards, medics), or NULL
static CTFPlayer *NearestBodyguard(CTFBot *me, float *dist)
{
	CTFPlayer *best = NULL;
	float bestD = FLT_MAX;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (!p || p == me || !p->IsAlive() || p->GetTeamNumber() != me->GetTeamNumber() || p->IsPlayerClass(TF_CLASS_CIVILIAN))
			continue;
		float d = (p->GetAbsOrigin() - me->GetAbsOrigin()).Length();
		if (d < bestD)
		{
			bestD = d;
			best = p;
		}
	}
	*dist = bestD;
	return best;
}

// ---------------------------------------------------------------------------------------------
// Assassin snipers
// ---------------------------------------------------------------------------------------------
bool CTFBotHuntedSniper::PickSpot(CTFBot *me)
{
	CTFPlayer *vip = PF2Hunted_FindVIP(HuntedVipTeam());
	Vector anchor = (vip && vip->IsAlive()) ? vip->GetAbsOrigin() : GoalPoint();
	CNavArea *anchorArea = TheNavMesh->GetNearestNavArea(anchor, false, 500.0f);
	CNavArea *fallback = NULL;
	for (int tries = 0; tries < 60 && TheNavAreas.Count(); ++tries)
	{
		CNavArea *a = TheNavAreas[RandomInt(0, TheNavAreas.Count() - 1)];
		if (a->IsBlocked(me->GetTeamNumber()) || a->IsUnderwater())
			continue;
		float d = (a->GetCenter() - anchor).Length();
		if (d < 700.0f || d > 1800.0f)
			continue;
		if (!anchorArea || a->IsPotentiallyVisible(anchorArea))
		{
			m_spot = a->GetRandomPoint();
			return true;
		}
		if (!fallback)
			fallback = a;
	}
	if (!fallback)
		return false;
	m_spot = fallback->GetRandomPoint();
	return true;
}

ActionResult<CTFBot> CTFBotHuntedSniper::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	m_haveSpot = false;
	m_arrived = false;
	return Continue();
}

ActionResult<CTFBot> CTFBotHuntedSniper::Update(CTFBot *me, float interval)
{
	if (!s_rules.active || !s_rules.hasGoal)
		return Done("Not a hunted map");
	// a target in sight: TFBot's sniper attack (zoom, aim, fire), then back to roaming
	if (CTFBotSniperAttack::IsPossible(me))
		return SuspendFor(new CTFBotSniperAttack, "Sniping a target");
	if (!m_haveSpot || (m_arrived && m_holdTimer.IsElapsed()) || (!m_arrived && m_travelTimer.IsElapsed()))
	{
		m_haveSpot = PickSpot(me);
		m_arrived = false;
		m_travelTimer.Start(25.0f);
		m_repathTimer.Invalidate();
		if (!m_haveSpot)
			return Continue();
	}
	if (!m_arrived)
	{
		if ((me->GetAbsOrigin() - m_spot).AsVector2D().IsLengthLessThan(60.0f))
		{
			m_arrived = true;
			m_holdTimer.Start(RandomFloat(12.0f, 20.0f));
		}
		else
		{
			if (m_repathTimer.IsElapsed())
			{
				m_repathTimer.Start(RandomFloat(1.0f, 2.0f));
				CTFBotPathCost cost(me, FASTEST_ROUTE);
				if (!m_path.Compute(me, m_spot, cost))
					m_haveSpot = false;   // unreachable: pick another next time
			}
			m_path.Update(me);
			return Continue();
		}
	}
	// hold the spot, watching the VIP's side (attacking is the main action's job)
	me->GetLocomotionInterface()->Stop();
	CTFPlayer *vip = PF2Hunted_FindVIP(HuntedVipTeam());
	Vector look = (vip && vip->IsAlive()) ? vip->WorldSpaceCenter() : GoalPoint() + Vector(0, 0, 40);
	me->GetBodyInterface()->AimHeadTowards(look, IBody::INTERESTING, 0.5f, NULL, "Watching for the VIP");
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotHuntedSniper::OnStuck(CTFBot *me)
{
	m_haveSpot = false;
	return TryContinue();
}

// ---------------------------------------------------------------------------------------------
// VIP
// ---------------------------------------------------------------------------------------------
ActionResult<CTFBot> CTFBotPF2HuntedVIP::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	m_repathTimer.Invalidate();
	m_routeSeed = (unsigned)RandomInt(1, 0x7fffffff);
	return Continue();
}

ActionResult<CTFBot> CTFBotPF2HuntedVIP::Update(CTFBot *me, float interval)
{
	if (!me->IsPlayerClass(TF_CLASS_CIVILIAN) || !s_rules.hasGoal)
		return Done("No longer the VIP");

	// only one VIP per map: if someone else (a human, or a lower-index bot) is also the Civilian,
	// step down; the class change applies on the next respawn
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (!p || p == me || p->GetTeamNumber() != me->GetTeamNumber() || !p->IsPlayerClass(TF_CLASS_CIVILIAN))
			continue;
		if (!p->IsBot() || p->entindex() < me->entindex())
		{
			me->HandleCommand_JoinClass(me->GetNextSpawnClassname());
			return ChangeTo(new CTFBotPF2HuntedBodyguard, "Another player is the VIP");
		}
	}

	if (!PF2Hunted_GateOpen())
	{
		// the bodyguards are opening the way: stay with them rather than waiting alone at the spawn
		float d;
		CTFPlayer *guard = NearestBodyguard(me, &d);
		if (guard && d > 200.0f)
		{
			if (m_repathTimer.IsElapsed())
			{
				m_repathTimer.Start(1.0f);
				CTFBotPathCost cost(me, FASTEST_ROUTE);
				m_path.Compute(me, guard->GetAbsOrigin(), cost);
			}
			m_path.Update(me);
		}
		else
			me->GetLocomotionInterface()->Stop();
		me->UpdateLookingAroundForIncomingPlayers(true);
		return Continue();
	}

	Vector goal = GoalPoint();
	bool inside = me->GetAbsOrigin().x >= s_rules.goalMins.x && me->GetAbsOrigin().x <= s_rules.goalMaxs.x &&
		me->GetAbsOrigin().y >= s_rules.goalMins.y && me->GetAbsOrigin().y <= s_rules.goalMaxs.y &&
		me->GetAbsOrigin().z >= s_rules.goalMins.z - 64.0f && me->GetAbsOrigin().z <= s_rules.goalMaxs.z;
	if (inside && s_rules.goalWait)
	{
		// e.g. Stonehenge: stand still on the lift; the map raises and teleports the VIP. Stand at the very
		// centre - a player hanging over the pillar's edge blocks the rising func_door, which then reverses.
		Vector centre((s_rules.goalMins.x + s_rules.goalMaxs.x) * 0.5f, (s_rules.goalMins.y + s_rules.goalMaxs.y) * 0.5f, me->GetAbsOrigin().z);
		if ((centre - me->GetAbsOrigin()).AsVector2D().IsLengthGreaterThan(8.0f))
			me->GetLocomotionInterface()->Approach(centre);
		else
			me->GetLocomotionInterface()->Stop();
		me->UpdateLookingAroundForIncomingPlayers(true);
		return Continue();
	}

	// FFNB: an escape zone the nav mesh does not cover (ff_hunted's ends ~140 units past the last
	// area): once close, walk straight in instead of stopping at the end of the mesh
	if (!inside && me->IsRangeLessThan(goal, 500.0f))
	{
		Vector into((s_rules.goalMins.x + s_rules.goalMaxs.x) * 0.5f, (s_rules.goalMins.y + s_rules.goalMaxs.y) * 0.5f, me->GetAbsOrigin().z);
		me->GetLocomotionInterface()->Approach(into);
		me->GetLocomotionInterface()->FaceTowards(into);
		return Continue();
	}

	// don't run off alone: when far ahead of the nearest bodyguard (who is further from the goal), wait
	// for a few seconds for the team to catch up - never walk back (a bodyguard holding a spot near the
	// spawn would pull the VIP to and fro) - then carry on and don't wait again for a while
	float guardDist;
	CTFPlayer *guard = NearestBodyguard(me, &guardDist);
	bool ahead = guard && guardDist > 900.0f && (goal - me->GetAbsOrigin()).IsLengthGreaterThan(600.0f) &&
		(goal - guard->GetAbsOrigin()).Length() > (goal - me->GetAbsOrigin()).Length();
	if (ahead && m_regroupCooldown.IsElapsed())
	{
		if (!m_regroupWait.HasStarted())
			m_regroupWait.Start(8.0f);
		if (!m_regroupWait.IsElapsed())
		{
			me->GetLocomotionInterface()->Stop();
			me->UpdateLookingAroundForIncomingPlayers(true);
			return Continue();
		}
		m_regroupWait.Invalidate();
		m_regroupCooldown.Start(20.0f);
	}
	else if (!ahead)
		m_regroupWait.Invalidate();
	if (m_repathTimer.IsElapsed())
	{
		m_repathTimer.Start(1.0f);
		HuntedRouteCost cost(me, m_routeSeed);
		m_path.Compute(me, goal, cost);
	}
	m_path.Update(me);
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotPF2HuntedVIP::OnStuck(CTFBot *me)
{
	m_repathTimer.Invalidate();
	return TryContinue();
}

// ---------------------------------------------------------------------------------------------
// Bodyguard
// ---------------------------------------------------------------------------------------------
ActionResult<CTFBot> CTFBotPF2HuntedBodyguard::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	m_repathTimer.Invalidate();
	m_button = -1;
	m_unreachable = 0;
	return Continue();
}

ActionResult<CTFBot> CTFBotPF2HuntedBodyguard::Update(CTFBot *me, float interval)
{
	if (!PF2Hunted_GateOpen() && s_rules.buttons.Count() > 0)
	{
		// spread the bodyguards over the buttons by entity index
		if (m_button < 0)
			m_button = me->entindex() % s_rules.buttons.Count();
		const Vector &button = s_rules.buttons[m_button];
		if (me->IsRangeLessThan(button, 72.0f))
		{
			// touch-activated buttons fire on contact; for use-buttons look at it and press +use
			me->GetBodyInterface()->AimHeadTowards(button, IBody::CRITICAL, 0.5f, NULL, "Pressing the gate button");
			me->PressUseButton(0.3f);
			m_path.Invalidate();
			// walk into it (touch) and try the next button after a moment
			me->GetLocomotionInterface()->Approach(button);
			if (RandomInt(0, 99) == 0)
				m_button = (m_button + 1) % s_rules.buttons.Count();
			return Continue();
		}
		if (m_repathTimer.IsElapsed() || !m_path.IsValid())
		{
			m_repathTimer.Start(2.0f);
			CTFBotPathCost cost(me, FASTEST_ROUTE);
			// Compute() also succeeds with a partial path to the closest area; a button whose path ends
			// far away (e.g. behind the gate itself) is unreachable: try another
			bool computed = m_path.Compute(me, button, cost);
			// (Compute() returns false when the goal point lies just outside the closest area, e.g. a button on the
			// wall; what matters is whether the path ends next to it)
			bool ok = m_path.IsValid() && (m_path.GetEndPosition() - button).IsLengthLessThan(160.0f);
			if (!ok)
			{
				static ConVarRef dbg("ffnb_debug");
				if (dbg.IsValid() && dbg.GetBool())
					Msg("[FFNB] %s: gate button %d (%.0f %.0f %.0f) unreachable: computed=%d valid=%d end=(%.0f %.0f %.0f)\n", me->GetPlayerName(),
						m_button, button.x, button.y, button.z, computed, m_path.IsValid(), m_path.GetEndPosition().x,
						m_path.GetEndPosition().y, m_path.GetEndPosition().z);
				m_path.Invalidate();
				m_button = (m_button + 1) % s_rules.buttons.Count();
				if (++m_unreachable >= s_rules.buttons.Count())
				{
					m_unreachable = 0;
					return SuspendFor(new CTFBotSeekAndDestroy(10.0f), "No reachable gate button; hunting assassins");
				}
				return Continue();
			}
			m_unreachable = 0;
		}
		m_path.Update(me);
		return Continue();
	}

	CTFPlayer *vip = PF2Hunted_FindVIP(s_vipTeam);
	if (vip && vip->IsAlive())
		return SuspendFor(new CTFBotEscort(vip), "Escorting the VIP");
	return SuspendFor(new CTFBotSeekAndDestroy(10.0f), "No VIP to escort; hunting assassins");
}

EventDesiredResult<CTFBot> CTFBotPF2HuntedBodyguard::OnStuck(CTFBot *me)
{
	m_repathTimer.Invalidate();
	if (s_rules.buttons.Count())
		m_button = (m_button + 1) % s_rules.buttons.Count();
	return TryContinue();
}

// ---------------------------------------------------------------------------------------------
// Nav tooling: the shipped meshes were generated without ladders, so platforms reachable only by a
// ladder (hunted tower buttons, sniper perches) have no areas. Seed the generator at every ladder top
// that has no area and run an incremental generate (+ full analysis) on a TEST server; the mesh is
// saved to addons/ffnextbot/nav (CNavMesh::GetFilename, PF2NB-patched). Bots are kicked meanwhile.
// ---------------------------------------------------------------------------------------------
extern ConVar nav_quicksave;
extern Vector NavTraceMins, NavTraceMaxs;

static bool LadderTopSeed(const CNavLadder *ladder, Vector *seed, Vector *normal)
{
	const Vector &top = ladder->m_top;
	for (int side = 0; side < 2; ++side)
	{
		NavDirType dir = side ? OppositeDirection(ladder->GetDir()) : ladder->GetDir();
		Vector center = top;
		AddDirectionVector(&center, dir, HalfHumanWidth);
		for (int d = -1; d < 2 * NUM_DIRECTIONS; ++d)
		{
			Vector p = center;
			if (d >= NUM_DIRECTIONS)
				AddDirectionVector(&p, (NavDirType)(d - NUM_DIRECTIONS), 2.0f * GenerationStepSize);
			else if (d >= 0)
				AddDirectionVector(&p, (NavDirType)d, GenerationStepSize);
			p.z += GenerationStepSize;
			p.x = TheNavMesh->SnapToGrid(p.x);
			p.y = TheNavMesh->SnapToGrid(p.y);
			Vector n;
			if (!TheNavMesh->GetGroundHeight(p, &p.z, &n) || fabs(p.z - top.z) > 48.0f)
				continue;   // the floor found is not the platform at the ladder top
			trace_t tr;
			UTIL_TraceHull(center + Vector(0, 0, 4), p + Vector(0, 0, 4), NavTraceMins, NavTraceMaxs,
				TheNavMesh->GetGenerationTraceMask(), NULL, COLLISION_GROUP_NONE, &tr);
			if (tr.fraction != 1.0f || tr.startsolid)
				continue;
			if (CNavArea *a = TheNavMesh->GetNavArea(p + Vector(0, 0, 8), 48.0f))
				return false;   // this ladder top is already covered
			*seed = p;
			*normal = n;
			return true;
		}
	}
	return false;
}

CON_COMMAND(ffnb_nav_seed_ladders, "ffnb_nav_seed_ladders [go]: list ladder tops without nav areas; 'go' = incremental generate from them (TEST servers only; kicks bots, saves the .nav)")
{
	if (!TheNavMesh || !TheNavMesh->IsLoaded())
	{
		Msg("[FFNB] no nav mesh loaded\n");
		return;
	}
	PF2_InvalidateEntityCache();
	bool go = args.ArgC() > 1 && !V_stricmp(args[1], "go");
	int seeds = 0;
	FOR_EACH_VEC(TheNavMesh->GetLadders(), i)
	{
		Vector p, n;
		if (!LadderTopSeed(TheNavMesh->GetLadders()[i], &p, &n))
			continue;
		Msg("[FFNB] ladder %d top (%.0f %.0f %.0f): seed (%.0f %.0f %.0f)\n", i, TheNavMesh->GetLadders()[i]->m_top.x,
			TheNavMesh->GetLadders()[i]->m_top.y, TheNavMesh->GetLadders()[i]->m_top.z, p.x, p.y, p.z);
		if (go)
		{
			if (!seeds)
				TheNavMesh->ClearWalkableSeeds();
			TheNavMesh->AddWalkableSeed(p, n);
		}
		++seeds;
	}
	int ladderSeeds = seeds;

	// player spawns with no nav nearby: e.g. spawn rooms behind doors that were shut when TF2's generator
	// sampled the map (plr_chariots: bots spawned off the mesh and never moved)
	CUtlVector<Vector> spawnSeeds;
	// FFNB: FF team spawns are info_ff_teamspawn; DM maps use info_player_start
	static const char *spawnClasses[] = { "info_ff_teamspawn", "info_player_*" };
	for (const char *sc : spawnClasses)
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, sc); e; e = gEntList.FindEntityByClassname(e, sc))
	{
		Vector pos = e->GetAbsOrigin();
		pos.z += HalfHumanHeight;
		Vector n;
		if (!TheNavMesh->GetGroundHeight(pos, &pos.z, &n))
			continue;
		if (TheNavMesh->GetNavArea(pos + Vector(0, 0, 8), 64.0f) || TheNavMesh->GetNearestNavArea(pos, false, 96.0f, false, false))
			continue;   // covered
		bool dup = false;
		FOR_EACH_VEC(spawnSeeds, s)
			if ((spawnSeeds[s] - pos).IsLengthLessThan(200.0f))
				dup = true;
		if (dup)
			continue;
		spawnSeeds.AddToTail(pos);
		pos.x = TheNavMesh->SnapToGrid(pos.x);
		pos.y = TheNavMesh->SnapToGrid(pos.y);
		Msg("[FFNB] spawn (%.0f %.0f %.0f) has no nav: seed\n", pos.x, pos.y, pos.z);
		if (go)
		{
			if (!seeds)
				TheNavMesh->ClearWalkableSeeds();
			TheNavMesh->AddWalkableSeed(pos, n);
		}
		++seeds;
	}
	Msg("[FFNB] %d uncovered ladder tops, %d uncovered spawns\n", ladderSeeds, spawnSeeds.Count());
	Msg("[FFNB] %d seeds\n", seeds);
	if (go && seeds)
	{
		TheNavMesh->BeginGeneration(true);
		nav_quicksave.SetValue(0);   // BeginGeneration(incremental) forces quicksave; we want hiding spots etc. back
		Msg("[FFNB] incremental generation started; the mesh is saved to addons/ffnextbot/nav when done\n");
	}
}

// Full generation seeded from EVERY team spawn (stock seeds from the first spawn point only, which leaves
// maps whose first spawn sits in a closed box with an empty mesh). TEST servers only: sets the bot quota
// to 0 and kicks the bots, generates and saves to addons/ffnextbot/nav; the map is NOT reloaded (do it after a humans check).
CON_COMMAND(ffnb_nav_generate, "Generate a nav mesh seeded from all team spawns (TEST servers only; kicks bots, saves to addons/ffnextbot/nav; reload the map yourself after a humans check)")
{
	if (!TheNavMesh)
		return;
	PF2_InvalidateEntityCache();
	engine->ServerCommand("ff_bot_quota 0\nff_bot_kick all\n");
	engine->ServerExecute();
	TheNavMesh->ClearWalkableSeeds();
	int seeds = 0;
	static const char *spawnClasses[] = { "info_ff_teamspawn", "info_player_*" };   // FFNB
	for (const char *sc : spawnClasses)
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, sc); e; e = gEntList.FindEntityByClassname(e, sc))
	{
		Vector pos = e->GetAbsOrigin();
		pos.x = TheNavMesh->SnapToGrid(pos.x);
		pos.y = TheNavMesh->SnapToGrid(pos.y);
		Vector normal;
		pos.z += HalfHumanHeight;   // spawn origins sit on (or just in) the floor
		if (TheNavMesh->GetGroundHeight(pos, &pos.z, &normal))
		{
			TheNavMesh->AddWalkableSeed(pos, normal);
			++seeds;
		}
	}
	Msg("[FFNB] %d walkable seeds from player spawns; generating\n", seeds);
	if (seeds)
		TheNavMesh->BeginGeneration(false);
}

// ---------------------------------------------------------------------------------------------
// FFNB: assassins (see ff_hunt_vip.h)
// ---------------------------------------------------------------------------------------------
ActionResult<CTFBot> CTFBotFFHuntVIP::OnStart(CTFBot *me, Action<CTFBot> *priorAction)
{
	m_repathTimer.Invalidate();
	m_path.SetMinLookAheadDistance(me->GetDesiredPathLookAheadRange());
	return Continue();
}

ActionResult<CTFBot> CTFBotFFHuntVIP::Update(CTFBot *me, float interval)
{
	if (!s_rules.active || !s_rules.hasGoal)
		return Done("Not a hunted map");
	if (m_repathTimer.IsElapsed())
	{
		m_repathTimer.Start(RandomFloat(1.0f, 2.0f));
		CTFPlayer *vip = PF2Hunted_FindVIP(s_vipTeam);
		// chase the VIP; with no VIP (or a dead one) wait on the way to the escape
		Vector target = (vip && vip->IsAlive()) ? vip->GetAbsOrigin() : GoalPoint();
		if (!(vip && vip->IsAlive()) && me->IsRangeLessThan(target, 300.0f))
			return Continue();
		CTFBotPathCost cost(me, FASTEST_ROUTE);
		m_path.Compute(me, target, cost);
	}
	m_path.Update(me);
	return Continue();
}

EventDesiredResult<CTFBot> CTFBotFFHuntVIP::OnStuck(CTFBot *me)
{
	m_repathTimer.Invalidate();
	return TryContinue();
}
