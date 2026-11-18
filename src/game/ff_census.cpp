// ff-nextbot: census of the map's Lua objectives (see ff_census.h).
#include "cbase.h"
#include "GameEventListener.h"
#include "ff_census.h"

void PF2_ForgetProxy(int index);   // shim/pf2_entity.cpp: recreate this entity's proxy on next use
void PF2NB_Log(const char *fmt, ...);
extern ConVar pf2nb_debug;

namespace ff {

static CUtlVector<CensusObj> s_objs;
static bool s_done = false;
static int s_teamOf[NUM_ENT_ENTRIES];
static short s_slotOf[NUM_ENT_ENTRIES];   // index into s_objs + 1, 0 = none

const char *Census_KindName(ObjKind k)
{
	static const char *n[] = { "none", "flag", "cap", "ball", "backpack", "hunted_escape", "respawndoor", "cpzone", "security", "other" };
	return (k >= 0 && k <= OBJ_OTHER) ? n[k] : "?";
}

// base_shutdown's security switches: "button_red" (func_button pressed by blue) or anything derived
// from "red_security_trigger" (touched by non-red). Returns the team whose security it turns off.
static const char *s_teamWords[6] = { NULL, NULL, "blue", "red", "yellow", "green" };
static bool HasToken(const char *lname, const char *lbase, const char *tok)
{
	size_t n = strlen(tok);
	if (!V_strcmp(lname, tok))
		return true;
	// ancestry is "a<b<c": match whole links only
	for (const char *p = lbase; (p = V_strstr(p, tok)) != NULL; p += n)
		if ((p == lbase || p[-1] == '<') && (p[n] == 0 || p[n] == '<'))
			return true;
	return false;
}
static int SecurityOwner(const char *lname, const char *lbase)
{
	for (int t = 2; t <= 5; ++t)
	{
		char a[48], b[48];
		V_snprintf(a, sizeof(a), "button_%s", s_teamWords[t]);
		V_snprintf(b, sizeof(b), "%s_security_trigger", s_teamWords[t]);
		if (HasToken(lname, lbase, a) || HasToken(lname, lbase, b))
			return t;
	}
	return -1;
}
// a trigger whose Lua class is not_<team>_trigger acts on everyone except <team> (base_teamplay)
static int SpareTeamOf(const char *lname, const char *lbase)
{
	for (int t = 2; t <= 5; ++t)
	{
		char a[48];
		V_snprintf(a, sizeof(a), "not_%s_trigger", s_teamWords[t]);
		if (HasToken(lname, lbase, a))
			return t;
	}
	return -1;
}

static ObjKind Classify(CensusObj &o)
{
	char lname[64], lbase[160];
	V_strncpy(lname, o.name, sizeof(lname)); V_strlower(lname);
	V_strncpy(lbase, o.base, sizeof(lbase)); V_strlower(lbase);
	bool infoScript = !V_stricmp(o.cls, "info_ff_script");
	int secOwner = infoScript ? -1 : SecurityOwner(lname, lbase);
	if (secOwner >= 2)
	{
		o.owner = secOwner;
		return OBJ_SECURITY;
	}
	o.spareTeam = SpareTeamOf(lname, lbase);
	if (V_strstr(lbase, "respawndoor"))
		return OBJ_RESPAWNDOOR;
	if (!infoScript && V_strstr(lbase, "cp_zone"))
		return OBJ_CPZONE;
	if (o.goal == GOAL_HUNTEDESCAPE || V_strstr(lbase, "hunted_escape") || V_strstr(lname, "hunted_escape"))
		return OBJ_HUNTED_ESCAPE;
	if (infoScript && (V_strstr(lname, "ball") || V_strstr(lbase, "ball")) && !V_strstr(lbase, "backpack"))
		return OBJ_BALL;
	if (o.goal == GOAL_FLAG || (infoScript && (V_strstr(lbase, "baseflag") || V_strstr(lbase, "_flag<"))))
		return OBJ_FLAG;
	if (o.goal == GOAL_FLAGCAP || V_strstr(lbase, "basecap") || V_strstr(lbase, "_cap<"))
		return OBJ_CAP;
	// a team's trigger that accepts carried items (fortball goals: item = "ball"); teamless ones
	// are returners / strippers (ball out of bounds), not goals
	if (!infoScript && o.items[0] && o.team >= 2 && !V_strstr(lbase, "returner") && !V_strstr(lname, "strip"))
		return OBJ_CAP;
	if ((o.goal >= GOAL_AMMO && o.goal <= GOAL_GRENADES) || V_strstr(lbase, "backpack") || o.packHealth || o.packArmor || o.packAmmo)
		return OBJ_BACKPACK;
	return OBJ_OTHER;
}

static bool s_secKnown[6], s_secUp[6];   // per team: base_shutdown security reported / currently on

static CensusObj *Slot(int ent, bool create)
{
	if (ent <= 0 || ent >= NUM_ENT_ENTRIES)
		return NULL;
	if (s_slotOf[ent])
		return &s_objs[s_slotOf[ent] - 1];
	if (!create)
		return NULL;
	CensusObj o;
	V_memset(&o, 0, sizeof(o));
	o.ent = ent;
	o.team = -1;
	o.owner = -1;
	o.spareTeam = -1;
	s_slotOf[ent] = (short)(s_objs.AddToTail(o) + 1);
	return &s_objs[s_slotOf[ent] - 1];
}

static int ParseTeam(const char *s)
{
	if (!s || !*s || !V_strncmp(s, "nil", 3))
		return -1;
	return atoi(s);
}

class CensusListener : public CGameEventListener
{
public:
	void FireGameEvent(IGameEvent *event) override
	{
		const char *ev = event->GetString("eventname");
		if (V_strncmp(ev, "ffnb_", 5))
			return;
		int ent = atoi(event->GetString("value0"));   // key0 == "ent" (userid must stay 0, see the script)
		const char *v1 = event->GetString("value1");
		const char *v2 = event->GetString("value2");
		if (!V_strcmp(ev, "ffnb_census_begin"))
		{
			// a (re)run of the census, e.g. Lua startup() again on round restart: start over
			FOR_EACH_VEC(s_objs, i)
				PF2_ForgetProxy(s_objs[i].ent);
			Census_Reset();
			return;
		}
		if (!V_strcmp(ev, "ffnb_obj"))
		{
			CensusObj *o = Slot(ent, true);
			if (!o)
				return;
			V_strncpy(o->name, v1, sizeof(o->name));
			// value2 = "<ancestry>|<classname>"
			char tmp[200];
			V_strncpy(tmp, v2, sizeof(tmp));
			char *bar = strrchr(tmp, '|');
			if (bar)
			{
				*bar = 0;
				V_strncpy(o->cls, bar + 1, sizeof(o->cls));
			}
			V_strncpy(o->base, tmp, sizeof(o->base));
		}
		else if (!V_strcmp(ev, "ffnb_objf"))
		{
			CensusObj *o = Slot(ent, true);
			if (!o)
				return;
			// value1 = "<team>|<botgoaltype>", value2 = item list
			// value1 = "<team>|<botgoaltype>|<touchmask>", value2 = item list or "pack:h=..,a=..,am=..,g=.."
			o->team = ParseTeam(v1);
			const char *bar = strchr(v1, '|');
			o->goal = bar ? atoi(bar + 1) : 0;
			const char *bar2 = bar ? strchr(bar + 1, '|') : NULL;
			o->touchMask = bar2 ? atoi(bar2 + 1) : -1;
			if (!V_strncmp(v2, "pack:", 5))
			{
				o->items[0] = 0;
				sscanf(v2 + 5, "h=%d,a=%d,am=%d,g=%d", &o->packHealth, &o->packArmor, &o->packAmmo, &o->packGrenades);
				if (o->kind == OBJ_NONE)
					o->goal = o->goal ? o->goal : GOAL_AMMO;
			}
			else
				V_strncpy(o->items, v2, sizeof(o->items));
			// a team-restricted object without a Lua team (spawn backpacks) belongs to its only team
			if (o->team < 0 && o->touchMask > 0)
				for (int t = 2; t <= 5; ++t)
					if (o->touchMask == (1 << t))
						o->team = t;
			o->kind = Classify(*o);
			s_teamOf[ent] = o->team;
			PF2_ForgetProxy(ent);   // the proxy type may change (info_ff_script -> flag, ...)
		}
		else if (!V_strcmp(ev, "ffnb_state"))
		{
			// value1 = "active" / "removed"
			if (CensusObj *o = Slot(ent, false))
				o->disabled = !V_strcmp(v1, "removed");
		}
		else if (!V_strcmp(ev, "ffnb_explode"))
		{
			// explosion-handling zones that only block grenades (nogrens, noannoyances) or tutorials are not targets
			CensusObj *o = Slot(ent, false);
			// only brush triggers are places a detpack goes (info_ff_script items like backpacks are not)
			if (o && V_stricmp(o->cls, "info_ff_script") && !V_stristr(o->base, "nogrens") && !V_stristr(o->base, "noannoyances") && !V_stristr(o->base, "training") &&
				!V_stristr(o->name, "nogrens") && !V_stristr(o->name, "noannoyances"))
				o->detTarget = true;
		}
		else if (!V_strcmp(ev, "ffnb_security"))
		{
			// value1 = team number, value2 = "up" / "down" (base_shutdown's button_<team>.sec_up)
			int t = atoi(v1);
			if (t >= 2 && t <= 5)
			{
				bool up = !V_strcmp(v2, "up");
				if (s_secKnown[t] && s_secUp[t] != up)
					PF2NB_Log("security of team %d is now %s", t, up ? "UP" : "DOWN");
				s_secKnown[t] = true;
				s_secUp[t] = !V_strcmp(v2, "up");
			}
		}
		else if (!V_strcmp(ev, "ffnb_owner"))
		{
			if (CensusObj *o = Slot(ent, false))
				o->owner = atoi(v1);
		}
		else if (!V_strcmp(ev, "ffnb_census_done"))
		{
			s_done = true;
			extern void PF2Hunted_FromCensus();
			PF2Hunted_FromCensus();
			int counts[OBJ_OTHER + 1] = { 0 };
			FOR_EACH_VEC(s_objs, i)
				counts[s_objs[i].kind]++;
			PF2NB_Log("census: %d objects (flags %d, caps %d, balls %d, backpacks %d, escapes %d, doors %d, security switches %d)", s_objs.Count(),
				counts[OBJ_FLAG], counts[OBJ_CAP], counts[OBJ_BALL], counts[OBJ_BACKPACK], counts[OBJ_HUNTED_ESCAPE], counts[OBJ_RESPAWNDOOR], counts[OBJ_SECURITY]);
		}
	}
};
static CensusListener s_listener;

ConVar ffnb_census_seq("ffnb_census_seq", "0", FCVAR_NONE, "Changing this makes the census script re-run (read by maps/globalscripts/ffnb_botinfo.lua)");
ConVar ffnb_census_script("ffnb_census_script", "1", FCVAR_NONE, "Set sv_globalluascript to ffnb_botinfo (the bot census) when it is empty");

// The census script must be loaded through sv_globalluascript, which FF reads in LevelInit before
// server.cfg runs on the first map, so the plugin sets it at load and checks it every map.
void Census_EnsureScript()
{
	if (!ffnb_census_script.GetBool())
		return;
	ConVar *g = g_pCVar->FindVar("sv_globalluascript");
	if (!g)
		return;
	if (!g->GetString()[0])
		g->SetValue("ffnb_botinfo");
	else if (V_stricmp(g->GetString(), "ffnb_botinfo"))
		PF2NB_Log("WARNING: sv_globalluascript is '%s'; the bot census needs ffnb_botinfo (objectives fall back to nothing)", g->GetString());
}

void Census_Register()
{
	s_listener.ListenForGameEvent("luaevent");
	Census_Reset();
	Census_EnsureScript();
	ffnb_census_seq.SetValue(ffnb_census_seq.GetInt() + 1);   // hot load: ask a running map for a census
}
void Census_Unregister() { s_listener.StopListeningForAllEvents(); }

void Census_Reset()
{
	s_objs.RemoveAll();
	s_done = false;
	V_memset(s_slotOf, 0, sizeof(s_slotOf));
	V_memset(s_secKnown, 0, sizeof(s_secKnown));
	V_memset(s_secUp, 0, sizeof(s_secUp));
	for (int i = 0; i < NUM_ENT_ENTRIES; ++i)
		s_teamOf[i] = -1;
}

bool Census_Done() { return s_done; }
int Census_Count() { return s_objs.Count(); }
const CensusObj *Census_At(int i) { return (i >= 0 && i < s_objs.Count()) ? &s_objs[i] : NULL; }
const CensusObj *Census_Get(int ent) { return (ent > 0 && ent < NUM_ENT_ENTRIES && s_slotOf[ent]) ? &s_objs[s_slotOf[ent] - 1] : NULL; }
bool Census_SecurityKnown(int team) { return team >= 2 && team <= 5 && s_secKnown[team]; }
bool Census_SecurityUp(int team) { return Census_SecurityKnown(team) && s_secUp[team]; }
int Census_SpareTeam(int ent) { const CensusObj *o = Census_Get(ent); return o ? o->spareTeam : -1; }
int Census_Team(int ent) { return (ent > 0 && ent < NUM_ENT_ENTRIES) ? s_teamOf[ent] : -1; }

bool Census_CapAccepts(const CensusObj *cap, const char *itemName)
{
	if (!cap || !itemName || !*itemName)
		return false;
	if (!cap->items[0])
		return true;   // no item list: any carried objective
	const char *p = cap->items;
	size_t n = strlen(itemName);
	while (p && *p)
	{
		if (!V_strncmp(p, itemName, n) && (p[n] == ',' || p[n] == 0))
			return true;
		p = strchr(p, ',');
		if (p)
			++p;
	}
	return false;
}

} // namespace ff

int FF_CensusTeamOf(int entindex) { return ff::Census_Team(entindex); }
int FF_CensusSpareTeam(int entindex) { return ff::Census_SpareTeam(entindex); }

CON_COMMAND(ffnb_census, "List the map's Lua objectives as reported by the census script")
{
	using namespace ff;
	Msg("[FFNB] census %s, %d objects\n", Census_Done() ? "complete" : "NOT received (is sv_globalluascript ffnb_botinfo set?)", Census_Count());
	for (int t = 2; t <= 5; ++t)
		if (Census_SecurityKnown(t))
			Msg("  security of team %d: %s\n", t, Census_SecurityUp(t) ? "UP" : "down");
	bool all = args.ArgC() > 1 && !V_stricmp(args[1], "all");
	for (int i = 0; i < Census_Count(); ++i)
	{
		const CensusObj *o = Census_At(i);
		if (!all && (o->kind == OBJ_OTHER || o->kind == OBJ_BACKPACK))
			continue;
		Msg("  #%-4d %-12s %-24s team %2d goal %d %s%s%s%s\n", o->ent, Census_KindName(o->kind), o->name, o->team, o->goal,
			o->disabled ? "[removed] " : "", (o->kind == OBJ_CPZONE || o->kind == OBJ_SECURITY) ? UTIL_VarArgs("owner %d ", o->owner) : "",
			o->items[0] ? "accepts " : "", o->items);
	}
}
