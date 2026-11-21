// pf2-nextbot: TFBot glue. Ties Valve's NextBot/TFBot (vendored in third_party/valve) to PF2:
//  * nav mesh: created via NavMeshFactory (CTFNavMesh), loaded on ServerActivate, updated per frame
//  * bots: the TFBot quota (CTFBotManager) requests bots; each request spawns one PF2 puppet bot;
//    when it appears a CTFBot is installed as that player slot's proxy and given a driver
//  * driving: PF2 calls PlayerRunCommand for each puppet every frame; the driver runs
//    NextBotPlayer::PhysicsSimulate (Update + input model) and writes the result into PF2's usercmd
//  * events: player_spawn/hurt/death and round start are fed to the bots as NextBot events
#include "cbase.h"
#include "plugin.h"
#include "botdriver.h"
#include "botmanager.h"
#include "nav_mesh.h"
#include "tf_nav_mesh.h"
#include "NextBotManager.h"
#include "bot/tf_bot.h"
#include "bot/tf_bot_manager.h"
#include "game/ff_census.h"
#include "perf.h"
extern int FF_TeamClassLimit(int team, int tfclass);
extern int FF_TeamClassCount(int team, int tfclass);
extern bool FF_TeamIsActive(int team);
#include "pf2_grenades.h"
#include "pf2_support.h"
#include "behaviors/pf2_hunted.h"

using namespace pf2;

CNavMesh *NavMeshFactory(void) { return new CTFNavMesh; }
extern ConVarRef developer, tf_avoidteammates, tf_avoidteammates_pushaway;
extern ConVar tf_bot_difficulty, tf_bot_quota, tf_bot_join_after_player;
extern const char *GetRandomBotName(void);
void PF2NB_StartStats();
void PF2NB_DoorsFrame();
void PF2NB_DoorsReset();
void PF2_RefreshControlPointMasters();
void PF2NB_HazardsFrame();
void PF2NB_HazardsReset();
void PF2NB_PruneDrops();
void PF2NB_ForgetDrops();

ConVar pf2nb_enable("ffnb_enable", "1", FCVAR_NONE, "Enable PF2 NextBot (TFBot) bots");
ConVar tf_bot_civilian_chance("ff_bot_civilian_chance", "10", FCVAR_NONE,
	"Percent chance a TFBot picks Civilian when choosing a class (only if pf_allow_special_class is 1, never on hunted/VIP maps)", true, 0, true, 100);
ConVar tf_bot_civilian_max_per_team("ff_bot_civilian_max_per_team", "1", FCVAR_NONE, "Most Civilian bots per team (random Civilian)");
ConVar pf2nb_debug("ffnb_debug", "0", FCVAR_NONE, "Log PF2 NextBot glue activity");
ConVar ffnb_spawnroom_radius("ffnb_spawnroom_radius", "600", FCVAR_NONE, "FF has no func_respawnroom: nav areas within this travel distance of a team spawn count as its spawn room");
ConVar pf2nb_unstick_jump("ffnb_unstick_jump", "1", FCVAR_NONE, "Bots stuck on the ground for 2+ seconds jump (steps the nav mesh does not mark as jumps)");

namespace pf2 { const char *Hibernation_Set(bool preventHibernation); }
// The engine must not go through a hibernate -> wake cycle with the patch half-applied: changing the
// patch while the server is asleep crashed the wake-up in testing. So the patch is only changed at
// boot (before the first map) or while clients are connected, and it is never undone on unload.
static bool s_hibernationWanted = true;
static bool s_hibernationApplied = false;
static void ApplyHibernationIfSafe(bool atBoot)
{
	if (s_hibernationWanted == s_hibernationApplied)
		return;
	if (!atBoot && UTIL_ClientsInGame() == 0)
		return;   // possibly hibernating; wait until someone is connected
	PF2NB_Log("%s", pf2::Hibernation_Set(s_hibernationWanted));
	s_hibernationApplied = s_hibernationWanted;
}
static void HibernationChanged(IConVar *var, const char *, float)
{
	s_hibernationWanted = ((ConVar *)var)->GetBool();
	ApplyHibernationIfSafe(false);
}
ConVar pf2nb_prevent_hibernation("ffnb_prevent_hibernation", "1", FCVAR_NONE,
	"Keep the server awake when no humans are connected so bots can play (like TF2's tf_allow_server_hibernation 0)", HibernationChanged);

static CTFBot *s_bots[PF2NB_MAXPLAYERS + 1];
static bool s_navLoaded = false;
static bool s_levelActive = false;
static bool s_warnedNoNav = false;   // told the players this map has no nav mesh (once per map)
static float s_nextAreaUpdate = 0.0f;

// ---------------------------------------------------------------------------------------------
// Driver: PlayerRunCommand -> NextBotPlayer::PhysicsSimulate -> PF2 usercmd
// ---------------------------------------------------------------------------------------------
class TFBotDriver : public IPF2BotDriver
{
public:
	explicit TFBotDriver(CTFBot *bot) : m_bot(bot) {}
	void OnRunCommand(PF2UserCmd *cmd) override
	{
		if (!m_bot->IsAlive())
		{
			// FFNB: FF respawns a dead player on a fresh button press (CBasePlayer::PlayerDeathThink);
			// nothing else presses for a fake client, so tap attack every other frame.
			cmd->buttons = (gpGlobals->tickcount & 1) ? IN_ATTACK : 0;
			return;
		}
		if (!pf2nb_enable.GetBool() || !s_navLoaded)
			return;
		double t0 = Plat_FloatTime();
		m_bot->UpdateLastKnownArea();
		m_bot->PhysicsSimulate();
		double dt = Plat_FloatTime() - t0;
		perf::Add(perf::THINK, dt);
		perf::MaxOf(perf::THINK_MAX_BOT, dt);
		CUserCmd built;
		if (!m_bot->TakePendingCmd(&built))
			return;
		QAngle view;
		cmd->viewangles = m_bot->GetDesiredView(&view) ? view : built.viewangles;
		cmd->forwardmove = built.forwardmove;
		cmd->sidemove = built.sidemove;
		cmd->upmove = built.upmove;
		cmd->buttons = built.buttons;
		cmd->impulse = built.impulse;
		PF2Grenades_Think(m_bot, cmd);   // PF2's prime-and-throw grenades
		PF2Support_Think(m_bot, cmd);    // medic syringe / engineer wrench on teammates
		{
			extern void FF_Abilities_Think(CTFBot *bot, CUserCmd *cmd);
			FF_Abilities_Think(m_bot, cmd);   // FF class skills on IN_ATTACK2 (pyro jetpack, ...)
		}
		// PF2 community maps have steps/stairs taller than a bot can walk up and meshes that don't mark
		// them as jumps; a bot stuck on the ground for a while hops (TFBot itself never does)
		{
			ILocomotion *loc = m_bot->GetLocomotionInterface();
			if (pf2nb_unstick_jump.GetBool() && loc->IsStuck() && loc->GetStuckDuration() > 2.0f && loc->IsOnGround() &&
				gpGlobals->curtime >= m_nextUnstickJump)
			{
				cmd->buttons |= IN_JUMP;
				m_nextUnstickJump = gpGlobals->curtime + 1.5f;
			}
		}
		m_lastButtons = cmd->buttons;
	}
	CTFBot *m_bot;
	int m_lastButtons = 0;
	float m_nextUnstickJump = 0.0f;
};
static TFBotDriver *s_drivers[PF2NB_MAXPLAYERS + 1];

static void UnbindBot(int idx)
{
	if (idx < 1 || idx > PF2NB_MAXPLAYERS)
		return;
	Drivers_Set(idx, NULL);
	delete s_drivers[idx];
	s_drivers[idx] = NULL;
	if (s_bots[idx])
	{
		extern void PF2_UninstallProxy(int index, CBaseEntity *proxy);
		PF2_UninstallProxy(idx, s_bots[idx]);
		PF2Grenades_Forget(s_bots[idx]);
		PF2Support_Forget(s_bots[idx]);
		delete s_bots[idx];   // INextBot dtor unregisters from TheNextBots()
		s_bots[idx] = NULL;
	}
}

// ---------------------------------------------------------------------------------------------
// Creation (called by CTFBotManager::MaintainBotQuota and tf_bot_add)
// ---------------------------------------------------------------------------------------------
// Requests are queued and issued one at a time. Team and class are resolved only when a request is
// issued, after the previous bot has finished joining, so PF2's team balance and TFBot's class roster
// see an up-to-date roster (the native code joins team and class synchronously).
struct BotRequest { char name[64]; int team; char cls[32]; int skill; bool quota; };
static CUtlVector<BotRequest> s_requests;
static int s_pendingBots = 0;        // issued puppet spawns not yet bound
static float s_lastBindTime = -1.0f;

int PF2NB_PendingBotCount() { return s_requests.Count() + s_pendingBots; }

// FFNB: the emptiest team the map's Lua enabled (2 to 4 teams), ties broken at random
static int ChooseAutoTeam()
{
	int best[TF_TEAM_COUNT], n = 0, fewest = 1 << 30;
	for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
	{
		CTeam *team = GetGlobalTeam(t);
		if (!team || !FF_TeamIsActive(t))
			continue;
		int c = team->GetNumPlayers();
		int *maxp = team->SendField<int>("m_iMaxPlayers");
		if (maxp && *maxp > 0 && c >= *maxp)
			continue;   // full (Hunted's VIP team takes one player)
		if (c < fewest)
		{
			fewest = c;
			n = 0;
		}
		if (c == fewest)
			best[n++] = t;
	}
	return n ? best[RandomInt(0, n - 1)] : (RandomInt(0, 1) ? TF_TEAM_RED : TF_TEAM_BLUE);
}

static bool AnyBotStillJoining()
{
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFBot *bot = s_bots[i];
		if (bot && (bot->GetTeamNumber() < FIRST_GAME_TEAM || bot->GetPlayerClass()->GetClassIndex() == TF_CLASS_UNDEFINED))
			if ((float)Plat_FloatTime() - s_lastBindTime < 3.0f)
				return true;
	}
	return false;
}

static void OnPuppetSpawned(int idx, void *ctx)
{
	BotRequest *p = (BotRequest *)ctx;
	--s_pendingBots;
	if (idx >= 1 && idx <= PF2NB_MAXPLAYERS && s_levelActive)
	{
		UnbindBot(idx);
		CTFBot *bot = new CTFBot;
		PF2_InstallProxy(idx, bot);
		bot->SetDifficulty((CTFBot::DifficultyType)clamp(p->skill, (int)CTFBot::EASY, (int)CTFBot::EXPERT));
		if (p->quota)
			bot->SetAttribute(CTFBot::QUOTA_MANANGED);
		s_bots[idx] = bot;
		s_drivers[idx] = new TFBotDriver(bot);
		Drivers_InstallHook(bot->GetRaw());
		Drivers_Set(idx, s_drivers[idx]);
		engine->SetFakeClientConVarValue(bot->edict(), "cl_autoreload", "1");
		s_lastBindTime = (float)Plat_FloatTime();
		if (pf2nb_debug.GetBool())
			PF2NB_Log("bound CTFBot to #%d (%s, %s %s)", idx, bot->GetPlayerName(), p->team == TF_TEAM_RED ? "red" : "blue", p->cls);
	}
	delete p;
}

bool PF2NB_ClassAllowedForTeam(int team, int tfclass);

static void IssueNextRequest()
{
	if (!s_requests.Count() || s_pendingBots > 0 || Puppet_Pending() > 0 || AnyBotStillJoining())
		return;
	BotRequest *r = new BotRequest(s_requests[0]);
	s_requests.Remove(0);
	if (r->team < FIRST_GAME_TEAM || r->team >= TF_TEAM_COUNT || !FF_TeamIsActive(r->team))
		r->team = ChooseAutoTeam();
	if (!r->cls[0])
	{
		// TFBot's own roster logic chooses, evaluated for the team this bot will join
		CTFBot *probe = new CTFBot;   // heap: entity classes expect zero-filled allocation
		probe->PF2_SetPendingTeam(r->team);
		probe->SetDifficulty((CTFBot::DifficultyType)clamp(r->skill, (int)CTFBot::EASY, (int)CTFBot::EXPERT));
		const char *c = probe->GetNextSpawnClassname();
		// FFNB: if TFBot found nothing, any class this team may play (Lua class limits), Civilian included
		const char *pick = (c && PF2_ClassFromName(c)) ? c : NULL;
		for (int tries = 0; !pick && tries < 32; ++tries)
		{
			int cand = RandomInt(TF_CLASS_SCOUT, TF_CLASS_CIVILIAN);
			if (PF2NB_ClassAllowedForTeam(r->team, cand))
				pick = PF2_ClassName(cand);
		}
		V_strncpy(r->cls, pick ? pick : "random", sizeof(r->cls));
		delete probe;
	}
	++s_pendingBots;
	static const char *teamNames[TF_TEAM_COUNT] = { "spec", "spec", "blue", "red", "yellow", "green" };   // FFNB
	Puppet_Request(teamNames[r->team], r->cls, r->name, OnPuppetSpawned, r);
}

void PF2NB_RequestTFBot(const char *name, int team, const char *classname, int skill, bool quotaManaged)
{
	if (!s_levelActive)
		return;
	BotRequest r;
	V_strncpy(r.name, name ? name : "TFBot", sizeof(r.name));
	r.team = team;
	V_strncpy(r.cls, classname ? classname : "", sizeof(r.cls));
	r.skill = skill;
	r.quota = quotaManaged;
	s_requests.AddToTail(r);
}

bool UTIL_KickBotFromTeam(int kickTeam)
{
	// prefer a dead bot, then any (CTFBotManager::MaintainBotQuota semantics)
	for (int pass = 0; pass < 2; ++pass)
		for (int i = gpGlobals->maxClients; i >= 1; --i)
		{
			CTFBot *bot = s_bots[i];
			if (!bot || !bot->HasAttribute(CTFBot::QUOTA_MANANGED) || bot->GetTeamNumber() != kickTeam)
				continue;
			if (pass == 0 && bot->IsAlive())
				continue;
			engine->ServerCommand(UTIL_VarArgs("kickid %d\n", bot->GetUserID()));
			return true;
		}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Class rules
// ---------------------------------------------------------------------------------------------
// Maps can forbid classes per team without any entity a bot can read easily (e.g. hunted maps kill
// the wrong classes with trigger_hurt + class filters at spawn). A class whose bots keep dying to the
// world right after spawning is learned as forbidden for that team until the map changes.
ConVar tf_bot_pf2_learn_class_restrictions("ff_bot_learn_class_restrictions", "1", FCVAR_NONE,
	"Stop picking a class whose bots repeatedly die to the world within seconds of spawning (per team, per map)");
static int s_spawnDeathStrikes[TF_TEAM_COUNT][TF_CLASS_COUNT_ALL];
static float s_spawnTime[PF2NB_MAXPLAYERS + 1];


bool PF2NB_ClassAllowedForTeam(int team, int tfclass)
{
	// FFNB: the map's Lua class limits come first (SetClassLimit / SetPlayerLimit)
	if (team >= FIRST_GAME_TEAM && team < TF_TEAM_COUNT)
	{
		int lim = FF_TeamClassLimit(team, tfclass);
		if (lim == -1)
			return false;
		if (lim > 0 && FF_TeamClassCount(team, tfclass) >= lim)
			return false;
		if (tfclass == TF_CLASS_CIVILIAN)
		{
			// a team that may only play Civilian (Hunted's VIP) gets one
			bool onlyCivilian = true;
			for (int c = TF_CLASS_SCOUT; c <= TF_CLASS_ENGINEER; ++c)
				if (FF_TeamClassLimit(team, c) != -1)
					onlyCivilian = false;
			if (onlyCivilian)
				return true;
		}
	}
	if (tfclass == TF_CLASS_CIVILIAN)
		return false;   // Civilian only through the random-Civilian / hunted-VIP rules
	bool hasRule = false;
	if (!PF2Hunted_ClassAllowed(team, tfclass, &hasRule))
		return false;   // hunted.cfg forbids it
	if (hasRule)
		return true;    // hunted.cfg allows it; no need for learning
	if (tfclass < TF_CLASS_SCOUT || tfclass > TF_CLASS_ENGINEER)
		return false;
	if (tf_bot_pf2_learn_class_restrictions.GetBool() && team >= 0 && team < TF_TEAM_COUNT && s_spawnDeathStrikes[team][tfclass] >= 2)
	{
		// never rule out every class for a team
		for (int c = TF_CLASS_SCOUT; c <= TF_CLASS_ENGINEER; ++c)
			if (c != tfclass && s_spawnDeathStrikes[team][c] < 2)
				return false;
	}
	return true;
}

static void NoteSpawnDeath(CTFPlayer *victim, bool byWorld)
{
	int idx = victim->entindex();
	if (!byWorld || idx < 1 || idx > PF2NB_MAXPLAYERS || gpGlobals->curtime - s_spawnTime[idx] > 10.0f)
		return;
	int team = victim->GetTeamNumber(), cls = victim->GetPlayerClass()->GetClassIndex();
	if (team < 0 || team >= TF_TEAM_COUNT || cls <= 0 || cls >= TF_CLASS_COUNT_ALL)
		return;
	if (++s_spawnDeathStrikes[team][cls] == 2)
		PF2NB_Log("map kills %s on team %d right after spawning; bots will not pick it on this map", PF2_ClassName(cls), team);
}

static void ResetClassLearning() { memset(s_spawnDeathStrikes, 0, sizeof(s_spawnDeathStrikes)); }

// Spawning inside an active damaging trigger (e.g. hunted maps' "cage" for wrong classes) means the
// map does not want this class on this team: one strike is enough.
static bool InsideActiveHurtTrigger(const Vector &pos)
{
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "trigger_hurt*"); e; e = gEntList.FindEntityByClassname(e, "trigger_hurt*"))
	{
		bool *disabled = e->Field<bool>("m_bDisabled");
		float *damage = e->Field<float>("m_flDamage");
		if ((disabled && *disabled) || (damage && *damage <= 0.0f))
			continue;
		if (e->CollisionProp()->IsPointInBounds(pos + Vector(0, 0, 16)))
			return true;
	}
	return false;
}

static void NoteSpawnPosition(CTFPlayer *bot)
{
	int team = bot->GetTeamNumber(), cls = bot->GetPlayerClass()->GetClassIndex();
	if (team < 0 || team >= TF_TEAM_COUNT || cls <= 0 || cls >= TF_CLASS_COUNT_ALL)
		return;
	if (tf_bot_pf2_learn_class_restrictions.GetBool() && s_spawnDeathStrikes[team][cls] < 2 &&
		InsideActiveHurtTrigger(bot->GetAbsOrigin()))
	{
		s_spawnDeathStrikes[team][cls] = 2;
		PF2NB_Log("%s spawned inside a damaging trigger on team %d; bots will not pick it on this map", PF2_ClassName(cls), team);
	}
	// a bot that spawned as a class its team may not play here (hunted rules, learned cages; also after the
	// map swapped teams) re-picks and respawns right away instead of dying in the cage every respawn wave
	bool allowed;
	if (cls == TF_CLASS_CIVILIAN)
	{
		// Civilian is never a regular pick, but a VIP / random Civilian is fine unless this team may not play it
		bool hasRule = false;
		allowed = PF2Hunted_ClassAllowed(team, cls, &hasRule) && s_spawnDeathStrikes[team][cls] < 2;
	}
	else
		allowed = PF2NB_ClassAllowedForTeam(team, cls);
	if (!allowed)
	{
		if (CTFBot *b = dynamic_cast<CTFBot *>((CBaseEntity *)bot))
		{
			const char *next = b->GetNextSpawnClassname();
			if (next && *next)
			{
				b->HandleCommand_JoinClass(next);
				if (b->GetPlayerClass()->GetClassIndex() != cls)
					b->ForceRespawn();
			}
		}
	}
}

bool PF2NB_ShouldPickCivilian(const CTFBot *bot)
{
	if (PF2Hunted_WantsBotAsVIP(bot))
		return true;   // hunted maps: this bot plays the VIP
	if (tf_bot_civilian_chance.GetFloat() <= 0.0f)
		return false;
	static ConVarRef special("pf_allow_special_class");
	if (!special.IsValid() || !special.GetBool())
		return false;
	if (TFGameRules()->IsHuntedOrVIPMap())
		return false;
	int team = bot->GetTeamNumber();
	int civs = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (p && p != bot && p->GetTeamNumber() == team && p->IsPlayerClass(TF_CLASS_CIVILIAN))
			++civs;
	}
	if (civs >= tf_bot_civilian_max_per_team.GetInt())
		return false;
	return RandomFloat(0.0f, 100.0f) < tf_bot_civilian_chance.GetFloat();
}

// ---------------------------------------------------------------------------------------------
// Game events -> NextBot events
// ---------------------------------------------------------------------------------------------
class PF2NBEvents : public CGameEventListener
{
public:
	void Start()
	{
		ListenForGameEvent("player_spawn");
		ListenForGameEvent("player_hurt");
		ListenForGameEvent("player_death");
		ListenForGameEvent("teamplay_round_start");
	}
	void FireGameEvent(IGameEvent *event) override
	{
		const char *name = event->GetName();
		CTFPlayer *victim = ToTFPlayer(UTIL_PlayerByUserId(event->GetInt("userid")));
		if (!V_strcmp(name, "player_spawn"))
		{
			if (victim)
			{
				victim->SetSpawnTime(gpGlobals->curtime);
				if (victim->entindex() >= 1 && victim->entindex() <= PF2NB_MAXPLAYERS)
					s_spawnTime[victim->entindex()] = gpGlobals->curtime;
			}
			CTFBot *bot = victim ? s_bots[victim->entindex()] : NULL;
			if (bot && bot == victim)
			{
				bot->Spawn();
				NoteSpawnPosition(victim);
			}
			return;
		}
		if (!V_strcmp(name, "teamplay_round_start"))
		{
			PF2_RefreshControlPointMasters();
			// hunted: the designated VIP bot switches to Civilian now rather than after its next death
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				CTFBot *b = s_bots[i];
				if (b && !b->IsPlayerClass(TF_CLASS_CIVILIAN) && PF2Hunted_WantsBotAsVIP(b))
				{
					b->HandleCommand_JoinClass("civilian");
					b->ForceRespawn();
					break;
				}
			}
			TheNextBots().OnRoundRestart();
			if (TheNavMesh && s_navLoaded)
				TheNavMesh->OnRoundRestart();
			return;
		}
		if (!victim)
			return;
		CTFPlayer *attacker = ToTFPlayer(UTIL_PlayerByUserId(event->GetInt("attacker")));
		CTakeDamageInfo info(attacker, attacker, (float)event->GetInt("damageamount", 1), DMG_GENERIC);
		if (!V_strcmp(name, "player_hurt"))
		{
			victim->SetTimeOfLastInjury(gpGlobals->curtime);
			CTFBot *bot = s_bots[victim->entindex()];
			if (bot && bot == victim)
				bot->OnTakeDamage_Alive(info);
		}
		else if (!V_strcmp(name, "player_death"))
		{
			CTFBot *bot = s_bots[victim->entindex()];
			if (bot && bot == victim)
			{
				NoteSpawnDeath(victim, attacker == NULL || attacker == victim);
				bot->Event_Killed(info);
			}
			TheNextBots().OnKilled(victim, info);
		}
	}
};
static PF2NBEvents s_events;

// ---------------------------------------------------------------------------------------------
// Lifecycle (called from plugin.cpp)
// ---------------------------------------------------------------------------------------------
namespace pf2 {

void BotManager_OnLoad(bool late)
{
	ff::Census_Register();
	// global ConVarRefs are constructed before the cvar interface exists; bind them now
	developer.Init("developer", true);
	tf_avoidteammates.Init("tf_avoidteammates", true);
	tf_avoidteammates_pushaway.Init("tf_avoidteammates_pushaway", true);
	PF2_LoadClassData();
	TheTFBots();   // instantiate the TFBot manager as the NextBot manager singleton
	if (!TheNavMesh)
		TheNavMesh = NavMeshFactory();
	s_events.Start();
	::PF2NB_StartStats();
	s_hibernationWanted = pf2nb_prevent_hibernation.GetBool();
	ApplyHibernationIfSafe(!late);
	if (late && gpGlobals && gpGlobals->mapname != NULL_STRING)
		BotManager_OnServerActivate();
}

void BotManager_OnUnload()
{
	ff::Census_Unregister();
	for (int i = 1; i <= PF2NB_MAXPLAYERS; ++i)
		UnbindBot(i);
	s_events.StopListeningForAllEvents();
	PF2_ResetProxies();
	s_levelActive = false;
	s_navLoaded = false;
}

void BotManager_OnLevelInit(const char *map)
{
	ff::Census_EnsureScript();
	s_levelActive = false;
	TFGameRules()->OnLevelInit();
}

void BotManager_OnServerActivate()
{
	s_levelActive = true;
	if (TheNavMesh)
	{
		TheNavMesh->Reset();
		NavErrorType err = TheNavMesh->Load();
		s_navLoaded = (err == NAV_OK);
		s_warnedNoNav = false;
		if (s_navLoaded)
		{
			TheNavMesh->OnServerActivate();
			PF2Hunted_LoadForMap(STRING(gpGlobals->mapname));
			::PF2NB_PruneDrops();   // impossible drops, walls and ledges (after the per-map ladders are in)
		}
		PF2NB_Log("nav mesh for %s: %s (%d areas)", STRING(gpGlobals->mapname), s_navLoaded ? "loaded" : "NOT AVAILABLE, bots disabled on this map",
			s_navLoaded ? TheNavMesh->GetNavAreaCount() : 0);
	}
	TheTFBots().OnMapLoaded();
	PF2_RefreshControlPointMasters();

	// after a plugin reload, adopt PF2 puppet bots that are already in the game
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		if (!p || s_bots[i] || !p->IsFakeClient() || p->IsHLTV())
			continue;
		BotRequest *r = new BotRequest;
		V_strncpy(r->name, p->GetPlayerName(), sizeof(r->name));
		r->team = p->GetTeamNumber();
		r->cls[0] = 0;
		r->skill = tf_bot_difficulty.GetInt();
		r->quota = true;
		++s_pendingBots;
		OnPuppetSpawned(i, r);
		if (s_bots[i] && p->IsAlive())
			s_bots[i]->Spawn();
		PF2NB_Log("adopted existing puppet bot #%d (%s)", i, s_bots[i] ? s_bots[i]->GetPlayerName() : "?");
	}
}

void BotManager_OnLevelShutdown()
{
	ff::Census_Reset();
	for (int i = 1; i <= PF2NB_MAXPLAYERS; ++i)
		UnbindBot(i);
	TheTFBots().LevelShutdown();
	::PF2NB_DoorsReset();
	::PF2NB_HazardsReset();
	::PF2NB_ForgetDrops();
	ResetClassLearning();
	PF2_ResetProxies();
	if (TheNavMesh)
		TheNavMesh->Reset();
	s_levelActive = false;
	s_navLoaded = false;
	s_pendingBots = 0;
	s_requests.RemoveAll();
}

// FFNB: keep active teams within one player of each other. Humans join and leave any team, and the
// quota only adds to the emptiest team, so teams can drift (blue 2 / green 4). Every few seconds, if
// the biggest team is 2+ players ahead of the smallest open team, remove one of its quota bots (a dead
// one if possible, after a grace period otherwise); the quota re-adds a bot, which joins the emptiest.
ConVar ff_bot_auto_balance("ff_bot_auto_balance", "1", FCVAR_NONE,
	"Even out team sizes by replacing a bot on the biggest team (0 = off)");
static void RebalanceTeams()
{
	static float s_next = 0.0f, s_unevenSince = -1.0f;
	float now = (float)Plat_FloatTime();
	if (!ff_bot_auto_balance.GetBool() || now < s_next)
		return;
	s_next = now + 3.0f;
	if (s_requests.Count() || s_pendingBots > 0 || Puppet_Pending() > 0 || AnyBotStillJoining())
		return;   // a bot is still being added; judge the teams once it is in
	int biggest = TEAM_INVALID, most = -1, fewest = 1 << 30;
	for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
	{
		CTeam *team = GetGlobalTeam(t);
		if (!team || !FF_TeamIsActive(t))
			continue;
		int c = team->GetNumPlayers();
		int *maxp = team->SendField<int>("m_iMaxPlayers");
		bool full = maxp && *maxp > 0 && c >= *maxp;   // e.g. Hunted's one-player VIP team
		if (!full && c < fewest)
			fewest = c;
		bool hasBot = false;
		for (int i = 1; i <= gpGlobals->maxClients && !hasBot; ++i)
			hasBot = s_bots[i] && s_bots[i]->GetTeamNumber() == t && s_bots[i]->HasAttribute(CTFBot::QUOTA_MANANGED);
		if (hasBot && c > most)
		{
			most = c;
			biggest = t;
		}
	}
	if (biggest == TEAM_INVALID || fewest == (1 << 30) || most - fewest < 2)
	{
		s_unevenSince = -1.0f;
		return;
	}
	if (s_unevenSince < 0.0f)
		s_unevenSince = now;
	for (int i = gpGlobals->maxClients; i >= 1; --i)
	{
		CTFBot *bot = s_bots[i];
		if (!bot || bot->GetTeamNumber() != biggest || !bot->HasAttribute(CTFBot::QUOTA_MANANGED))
			continue;
		// wait for a dead bot so nobody vanishes mid-fight; after 20 s take any
		if (bot->IsAlive() && now - s_unevenSince < 20.0f)
			continue;
		PF2NB_Log("auto balance: team %d has %d players vs %d, replacing %s", biggest, most, fewest, bot->GetPlayerName());
		engine->ServerCommand(UTIL_VarArgs("kickid %d\n", bot->GetUserID()));
		s_unevenSince = -1.0f;
		s_next = now + 6.0f;   // let the quota re-add before judging again
		return;
	}
}

static int s_frames = 0, s_simFrames = 0;
void BotManager_OnGameFrame(bool simulating)
{
	++s_frames;
	if (simulating) ++s_simFrames;
	ApplyHibernationIfSafe(false);
	Puppet_Frame();
	if (s_levelActive && s_navLoaded)
		IssueNextRequest();
	if (!s_levelActive || !pf2nb_enable.GetBool())
		return;
	if (TheNavMesh && TheNavMesh->IsGenerating())
	{
		// the old areas are gone: forget door/hazard blocks without touching them
		::PF2NB_DoorsReset();
		::PF2NB_HazardsReset();
		::PF2NB_ForgetDrops();
		// keep the generator stepping even with no mesh loaded (s_navLoaded false) or no bots (not simulating)
		PF2_InvalidateEntityCache();
		TheNavMesh->Update();
		return;
	}
	if (!s_navLoaded && !s_warnedNoNav && tf_bot_quota.GetInt() > 0 && UTIL_ClientsInGame() > 0)
	{
		// Without a mesh the quota adds nobody and ff_bot_add refuses; say so where players can see it
		// (the refusal itself only reaches the server console).
		s_warnedNoNav = true;
		PF2NB_Log("no nav mesh for %s: bots can't play this map", STRING(gpGlobals->mapname));
		engine->ServerCommand(UTIL_VarArgs("say [Bots] No nav mesh for %s yet, so bots can't play this map.\n",
			STRING(gpGlobals->mapname)));
	}
	if (!simulating)
	{
		// An empty server does not simulate (no clients) and curtime stands still, so TFBot's quota
		// would never add the first bot. Seed it directly; once a bot is in, simulation starts.
		static float s_nextSeed = 0.0f;
		float now = (float)Plat_FloatTime();
		if (s_navLoaded && now >= s_nextSeed && tf_bot_quota.GetInt() > 0 && !tf_bot_join_after_player.GetBool() &&
			PF2NB_PendingBotCount() == 0 && UTIL_ClientsInGame() == 0)
		{
			s_nextSeed = now + 2.0f;
			PF2NB_RequestTFBot(GetRandomBotName(), TEAM_ANY, NULL, tf_bot_difficulty.GetInt(), true);
		}
		return;
	}
	if (s_navLoaded)
	{
		RebalanceTeams();
		TheNavMesh->Update();
		// keep everyone's last known nav area current (bots read it for enemies and teammates)
		if (gpGlobals->curtime >= s_nextAreaUpdate || gpGlobals->curtime < s_nextAreaUpdate - 1.0f)
		{
			s_nextAreaUpdate = gpGlobals->curtime + 0.2f;
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				CBasePlayer *p = UTIL_PlayerByIndex(i);
				if (p && p->IsAlive() && !s_bots[i])
					p->UpdateLastKnownArea();
			}
		}
		TheTFBots().Update();   // quota (MaintainBotQuota) + NextBotManager scheduling
		::PF2NB_DoorsFrame();
		::PF2NB_HazardsFrame();
	}
}

void BotManager_OnClientPutInServer(edict_t *ent, const char *name) {}

void BotManager_OnClientDisconnect(edict_t *ent)
{
	UnbindBot(engine->IndexOfEdict(ent));
}

} // namespace pf2

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------
CON_COMMAND(ffnb_status, "Show PF2 NextBot state")
{
	PF2NB_Log("enabled=%d nav=%s (%d areas) hook=%s pending=%d queued=%d puppet=%d | frames=%d sim=%d curtime=%.1f level=%d clients=%d",
		pf2nb_enable.GetInt(), s_navLoaded ? "loaded" : "none", TheNavMesh ? TheNavMesh->GetNavAreaCount() : 0,
		Drivers_HookHealthy() ? "ok" : "BROKEN", s_pendingBots, s_requests.Count(), Puppet_Pending(), pf2::s_frames, pf2::s_simFrames,
		gpGlobals->curtime, (int)s_levelActive, UTIL_ClientsInGame());
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFBot *bot = s_bots[i];
		if (!bot)
			continue;
		Vector o = bot->GetAbsOrigin();
		CNavArea *area = bot->GetLastKnownArea();
		const char *stack = "";
		if (IIntention *intent = bot->GetIntentionInterface())
			if (Behavior<CTFBot> *b = dynamic_cast<Behavior<CTFBot> *>(intent->FirstContainedResponder()))
				stack = b->PF2_DebugString();
		PF2NB_Log("#%d %-18s t=%d c=%d hp=%d alive=%d area=%d pos=(%.0f %.0f %.0f) | %s", i, bot->GetPlayerName(),
			bot->GetTeamNumber(), bot->GetPlayerClass()->GetClassIndex(), bot->GetHealth(), (int)bot->IsAlive(),
			area ? (int)area->GetID() : -1, o.x, o.y, o.z, stack);
		if (args.ArgC() > 1)
		{
			ILocomotion *loc = bot->GetLocomotionInterface();
			const PathFollower *path = bot->GetCurrentPath();
			const Path::Segment *goal = path && path->IsValid() ? path->GetCurrentGoal() : NULL;
			Vector v = bot->GetAbsVelocity();
			PF2NB_Log("    stuck=%d (%.1fs) speed=%.0f desired=%.0f maxspeed=%.0f path=%s len=%.0f goal=(%.0f %.0f %.0f) goalArea=%d weapon=%s",
				(int)loc->IsStuck(), loc->GetStuckDuration(), v.Length2D(), loc->GetDesiredSpeed(), bot->MaxSpeed(),
				path ? (path->IsValid() ? "valid" : "invalid") : "none", path && path->IsValid() ? path->GetLength() : 0.0f,
				goal ? goal->pos.x : 0, goal ? goal->pos.y : 0, goal ? goal->pos.z : 0, goal && goal->area ? (int)goal->area->GetID() : -1,
				bot->GetActiveWeapon() ? bot->GetActiveWeapon()->GetClassname() : "none");
			IVision *vis = bot->GetVisionInterface();
			const CKnownEntity *threat = vis ? vis->GetPrimaryKnownThreat() : NULL;
			CBaseEntity *te = threat ? threat->GetEntity() : NULL;
			int known = 0;
			if (vis)
			{
				CUtlVector<CKnownEntity> list;
				vis->CollectKnownEntities(&list);
				known = list.Count();
			}
			QAngle view;
			bool hasView = bot->GetDesiredView(&view);
			PF2NB_Log("    known=%d threat=%s #%d visible=%d dist=%.0f fov=%.0f view=(%.0f %.0f)%s firing=%d", known,
				te ? te->GetClassname() : "none", te ? te->entindex() : -1, threat ? (int)threat->IsVisibleRecently() : 0,
				te ? (te->GetAbsOrigin() - bot->GetAbsOrigin()).Length() : 0.0f, vis ? vis->GetFieldOfView() : 0.0f,
				view.x, view.y, hasView ? "" : "(none)", (s_drivers[i] && (s_drivers[i]->m_lastButtons & IN_ATTACK)) ? 1 : 0);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Nav / gameplay diagnostics
// ---------------------------------------------------------------------------------------------
#include "tf_nav_area.h"
static int s_grenadesThrown = 0, s_botKills = 0, s_botDeaths = 0;

class PF2NBStats : public CGameEventListener
{
public:
	void Start() { ListenForGameEvent("grenade_thrown"); ListenForGameEvent("player_death"); }
	void FireGameEvent(IGameEvent *event) override
	{
		if (!V_strcmp(event->GetName(), "grenade_thrown"))
			++s_grenadesThrown;
		else
		{
			CBasePlayer *a = UTIL_PlayerByUserId(event->GetInt("attacker"));
			CBasePlayer *v = UTIL_PlayerByUserId(event->GetInt("userid"));
			if (a && a != v && a->MyNextBotPointer()) ++s_botKills;
			if (v && v->MyNextBotPointer()) ++s_botDeaths;
		}
	}
};
static PF2NBStats s_stats;
void PF2NB_StartStats() { s_stats.Start(); }

CON_COMMAND(ffnb_area, "ffnb_area <id>: nav area extent, attributes, blocked state per team and its connections")
{
	if (args.ArgC() < 2 || !TheNavMesh)
		return;
	CTFNavArea *a = (CTFNavArea *)TheNavMesh->GetNavAreaByID(atoi(args[1]));
	if (!a)
	{
		Msg("[FFNB] no area %s\n", args[1]);
		return;
	}
	Extent e;
	a->GetExtent(&e);
	Msg("[FFNB] area %u (%.0f %.0f %.0f)-(%.0f %.0f %.0f) attr=0x%x anyTF=%d blocked blue=%d red=%d\n", a->GetID(), e.lo.x, e.lo.y, e.lo.z,
		e.hi.x, e.hi.y, e.hi.z, a->GetAttributes(), (a->HasAttributeTF(0xFFFFFFFF) ? 1 : 0), a->IsBlocked(TF_TEAM_BLUE), a->IsBlocked(TF_TEAM_RED));
	{
		// repeat CNavArea::UpdateBlocked's trace and name what it starts solid in
		Vector origin = a->GetCenter();
		origin.z += HalfHumanHeight;
		float sx = MAX(1, MIN(a->GetSizeX() / 2 - 5, HalfHumanWidth)), sy = MAX(1, MIN(a->GetSizeY() / 2 - 5, HalfHumanWidth));
		CTraceFilterWalkableEntities filter(NULL, COLLISION_GROUP_PLAYER_MOVEMENT, WALK_THRU_DOORS | WALK_THRU_BREAKABLES);
		trace_t tr;
		UTIL_TraceHull(origin, origin, Vector(-sx, -sy, 0), Vector(sx, sy, VEC_DUCK_HULL_MAX.z - HalfHumanHeight), MASK_NPCSOLID_BRUSHONLY, &filter, &tr);
		Msg("   block trace: startsolid=%d ent=#%d %s '%s'\n", tr.startsolid ? 1 : 0, tr.m_pEnt ? tr.m_pEnt->entindex() : -1,
			tr.m_pEnt ? tr.m_pEnt->GetClassname() : "-", tr.m_pEnt ? STRING(tr.m_pEnt->GetEntityName()) : "");
	}
	static const char *dirs[] = { "N", "E", "S", "W" };
	for (int d = 0; d < NUM_DIRECTIONS; ++d)
	{
		const NavConnectVector *c = a->GetAdjacentAreas((NavDirType)d);
		FOR_EACH_VEC((*c), i)
		{
			CNavArea *b = c->Element(i).area;
			Msg("   %s -> %u (%.0f %.0f %.0f)%s\n", dirs[d], b->GetID(), b->GetCenter().x, b->GetCenter().y, b->GetCenter().z,
				b->IsConnected(a, NUM_DIRECTIONS) ? "" : " one-way");
		}
	}
}

CON_COMMAND(ffnb_path, "ffnb_path <botidx>: the bot's velocity, stuck state and current path (first segments, goal)")
{
	int idx = args.ArgC() > 1 ? atoi(args[1]) : 0;
	CTFBot *bot = (idx >= 1 && idx <= PF2NB_MAXPLAYERS) ? s_bots[idx] : NULL;
	if (!bot)
		return;
	ILocomotion *loc = bot->GetLocomotionInterface();
	Vector v = loc->GetVelocity();
	Vector f = loc->GetFeet();
	PF2NB_Log("#%d feet (%.0f %.0f %.0f) vel (%.0f %.0f %.0f) speed %.0f stuck=%d (%.1fs) ground=%d area=%d", idx, f.x, f.y, f.z,
		v.x, v.y, v.z, v.Length2D(), loc->IsStuck(), loc->IsStuck() ? loc->GetStuckDuration() : 0.0f, loc->IsOnGround(),
		bot->GetLastKnownArea() ? bot->GetLastKnownArea()->GetID() : -1);
	const PathFollower *path = bot->GetCurrentPath();
	if (!path || !path->IsValid())
	{
		PF2NB_Log("  no current path");
		return;
	}
	const Path::Segment *goal = path->GetCurrentGoal();
	if (goal)
		PF2NB_Log("  goal (%.0f %.0f %.0f) area %d type %d, path length %.0f", goal->pos.x, goal->pos.y, goal->pos.z,
			goal->area ? goal->area->GetID() : -1, (int)goal->type, path->GetLength());
	int n = 0;
	for (const Path::Segment *s = path->FirstSegment(); s && n < 8; s = path->NextSegment(s), ++n)
		PF2NB_Log("  seg %d (%.0f %.0f %.0f) area %d type %d", n, s->pos.x, s->pos.y, s->pos.z, s->area ? s->area->GetID() : -1, (int)s->type);
}

CON_COMMAND(ffnb_navat, "ffnb_navat x y z [botidx]: nearest nav area to a point; with a bot, whether a path reaches it")
{
	if (!TheNavMesh || !s_navLoaded || args.ArgC() < 4)
	{
		PF2NB_Log("usage: pf2nb_navat x y z [botidx] (needs a nav mesh)");
		return;
	}
	PF2_InvalidateEntityCache();
	Vector p(atof(args[1]), atof(args[2]), atof(args[3]));
	CNavArea *a = TheNavMesh->GetNearestNavArea(p, false, 500.0f, false, false);
	if (!a)
	{
		PF2NB_Log("no area within 500 of (%.0f %.0f %.0f)", p.x, p.y, p.z);
		return;
	}
	Vector c = a->GetCenter();
	int lads = 0;
	FOR_EACH_VEC(TheNavMesh->GetLadders(), i)
		if ((TheNavMesh->GetLadders()[i]->m_top - p).AsVector2D().IsLengthLessThan(400.0f))
			++lads;
	PF2NB_Log("area %d centre=(%.0f %.0f %.0f) dist=%.0f ladders-within-400=%d", a->GetID(), c.x, c.y, c.z, (c - p).Length(), lads);
	if (args.ArgC() > 4)
	{
		CTFBot *bot = ToTFBot(UTIL_PlayerByIndex(atoi(args[4])));
		CNavArea *from = bot ? bot->GetLastKnownArea() : NULL;
		if (!from)
		{
			PF2NB_Log("bot has no area");
			return;
		}
		CNavArea *closest = NULL;
		ShortestPathCost cost;
		bool ok = NavAreaBuildPath(from, a, &p, cost, &closest);
		Vector cc = closest ? closest->GetCenter() : vec3_origin;
		PF2NB_Log("from area %d: %s; closest area %d (%.0f %.0f %.0f)", from->GetID(), ok ? "REACHABLE" : "unreachable",
			closest ? closest->GetID() : -1, cc.x, cc.y, cc.z);
		// the bot's own path cost (CTFBotPathCost), and why its first steps are refused
		if (CTFBot *tb = dynamic_cast<CTFBot *>(bot))
		{
			CNavArea *closest2 = NULL;
			CTFBotPathCost botCost(tb, FASTEST_ROUTE);
			bool ok2 = NavAreaBuildPath(from, a, &p, botCost, &closest2, 0.0f, tb->GetTeamNumber());
			PF2NB_Log("  with the bot's path cost: %s; closest area %d", ok2 ? "REACHABLE" : "unreachable", closest2 ? closest2->GetID() : -1);
			for (int d = 0; d < NUM_DIRECTIONS; ++d)
			{
				const NavConnectVector *c = from->GetAdjacentAreas((NavDirType)d);
				FOR_EACH_VEC((*c), i)
				{
					CNavArea *n = c->Element(i).area;
					PF2NB_Log("  -> %u: cost %.0f traversable=%d blocked=%d bad=%d", n->GetID(), botCost(n, from, NULL, NULL, -1.0f),
						tb->GetLocomotionInterface()->IsAreaTraversable(n) ? 1 : 0, n->IsBlocked(tb->GetTeamNumber()) ? 1 : 0,
						PF2NB_IsBadConnection(from, n, tb->GetTeamNumber()) ? 1 : 0);
				}
			}
		}
	}
}

CON_COMMAND(ffnb_botcmd, "ffnb_botcmd <botidx> <client command...>: run a client command as a bot (debugging)")
{
	if (args.ArgC() < 3)
		return;
	PF2_InvalidateEntityCache();
	CTFBot *b = ToTFBot(UTIL_PlayerByIndex(atoi(args[1])));
	if (!b)
	{
		PF2NB_Log("no bot %s", args[1]);
		return;
	}
	const char *cmd = args.ArgS();
	while (*cmd && *cmd != ' ')
		++cmd;   // skip the index
	while (*cmd == ' ')
		++cmd;
	b->ClientCommand(cmd);
	PF2NB_Log("#%s ran '%s'", args[1], cmd);
}

CON_COMMAND(ffnb_placement, "For each engineer: active weapon, blueprint, IsPlacementPosValid result, eye angles")
{
	PF2_InvalidateEntityCache();
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (!p || !p->IsPlayerClass(TF_CLASS_ENGINEER) || !p->IsAlive())
			continue;
		CTFWeaponBase *w = p->m_Shared.GetActiveTFWeapon();
		CTFWeaponBuilder *b = dynamic_cast<CTFWeaponBuilder *>(w);
		CBaseEntity *bp = b ? b->GetPlacementObject() : NULL;
		QAngle eye = p->EyeAngles();
		Vector o = bp ? bp->GetAbsOrigin() : vec3_origin;
		PF2NB_Log("#%d weapon=%s builderType=%d blueprint=%s #%d at (%.0f %.0f %.0f) valid=%d eye=(%.0f %.0f) pos=(%.0f %.0f %.0f) inSpawnRoom=%d", i,
			w ? w->GetClassname() : "-", b ? b->GetType() : -1, bp ? bp->GetClassname() : "-", bp ? bp->entindex() : -1,
			o.x, o.y, o.z, b ? (int)b->IsValidPlacement() : -1, eye.x, eye.y, p->GetAbsOrigin().x, p->GetAbsOrigin().y, p->GetAbsOrigin().z, (int)PointInRespawnRoom(p, p->WorldSpaceCenter(), false));
	}
}

CON_COMMAND(ffnb_cpinfo,"Control points: trigger extents, nav areas per point (count + centroid), owner/locked, and each bot's chosen point")
{
	PF2_InvalidateEntityCache();
	for (CBaseEntity *t = gEntList.FindEntityByClassname(NULL, "trigger_capture_area*"); t; t = gEntList.FindEntityByClassname(t, "trigger_capture_area*"))
	{
		CTeamControlPoint *p = ((CTriggerAreaCapture *)t)->GetControlPoint();
		Extent e;
		e.Init(t);
		int idx = p ? p->GetPointIndex() : -1;
		const CUtlVector<CTFNavArea *> *areas = (idx >= 0 && TheTFNavMesh()) ? TheTFNavMesh()->GetControlPointAreas(idx) : NULL;
		Vector c(0, 0, 0);
		int n = areas ? areas->Count() : 0;
		for (int i = 0; i < n; ++i)
			c += areas->Element(i)->GetCenter();
		if (n)
			c /= (float)n;
		PF2NB_Log("trigger '%s' -> point %d '%s' owner=%d locked=%d extent=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) areas=%d centroid=(%.0f %.0f %.0f)",
			STRING(t->GetEntityName()), idx, p ? STRING(p->GetEntityName()) : "-", p ? p->GetOwner() : -1, p ? (int)p->IsLocked() : -1,
			e.lo.x, e.lo.y, e.lo.z, e.hi.x, e.hi.y, e.hi.z, n, c.x, c.y, c.z);
	}
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFBot *b = s_bots[i];
		if (!b || !b->IsAlive())
			continue;
		CTeamControlPoint *p = b->GetMyControlPoint();
		PF2NB_Log("#%d team %d -> point %d '%s'", i, b->GetTeamNumber(), p ? p->GetPointIndex() : -1, p ? STRING(p->GetEntityName()) : "-");
	}
}

CON_COMMAND(ffnb_hull,"ffnb_hull x y z: player-hull traces at a point (in solid? which contents?) and 64u steps around it")
{
	if (args.ArgC() < 4)
		return;
	PF2_InvalidateEntityCache();
	Vector p(atof(args[1]), atof(args[2]), atof(args[3]));
	Vector mins(-24, -24, 0), maxs(24, 24, 82);
	unsigned masks[2] = { MASK_PLAYERSOLID_BRUSHONLY, MASK_PLAYERSOLID };
	for (int m = 0; m < 2; ++m)
	{
		trace_t tr;
		UTIL_TraceHull(p + Vector(0, 0, 2), p + Vector(0, 0, 2), mins, maxs, masks[m], NULL, COLLISION_GROUP_PLAYER_MOVEMENT, &tr);
		PF2NB_Log("%s: startsolid=%d allsolid=%d contents=%x ent=%s", m ? "playersolid" : "brushonly", tr.startsolid, tr.allsolid,
			tr.contents, tr.m_pEnt ? tr.m_pEnt->GetClassname() : "-");
		static const Vector dirs[4] = { Vector(64, 0, 0), Vector(-64, 0, 0), Vector(0, 64, 0), Vector(0, -64, 0) };
		for (int d = 0; d < 4; ++d)
		{
			UTIL_TraceHull(p + Vector(0, 0, 2), p + Vector(0, 0, 2) + dirs[d], mins, maxs, masks[m], NULL, COLLISION_GROUP_PLAYER_MOVEMENT, &tr);
			PF2NB_Log("   step (%+.0f %+.0f): fraction=%.2f contents=%x ent=%s", dirs[d].x, dirs[d].y, tr.fraction, tr.contents,
				tr.m_pEnt ? tr.m_pEnt->GetClassname() : "-");
		}
	}
	float h = 0;
	Vector n(0, 0, 0);
	bool g = TheNavMesh && TheNavMesh->GetGroundHeight(p + Vector(0, 0, 36), &h, &n);
	PF2NB_Log("ground: %d z=%.1f normal=(%.2f %.2f %.2f)", g, h, n.x, n.y, n.z);
}

CON_COMMAND(ffnb_navinfo,"Show TF nav mesh decoration (spawn rooms, incursion, control points) and bot stats")
{
	if (!TheNavMesh || !s_navLoaded)
	{
		PF2NB_Log("no nav mesh loaded");
		return;
	}
	int red = 0, blue = 0, incR = 0, incB = 0, cp = 0, blocked = 0, total = 0;
	FOR_EACH_VEC(TheNavAreas, it)
	{
		CTFNavArea *a = static_cast<CTFNavArea *>(TheNavAreas[it]);
		++total;
		if (a->HasAttributeTF(TF_NAV_SPAWN_ROOM_RED)) ++red;
		if (a->HasAttributeTF(TF_NAV_SPAWN_ROOM_BLUE)) ++blue;
		if (a->HasAttributeTF(TF_NAV_CONTROL_POINT)) ++cp;
		if (a->HasAttributeTF(TF_NAV_BLOCKED)) ++blocked;
		if (a->GetIncursionDistance(TF_TEAM_RED) >= 0.0f) ++incR;
		if (a->GetIncursionDistance(TF_TEAM_BLUE) >= 0.0f) ++incB;
	}
	PF2NB_Log("areas=%d spawnroom red=%d blue=%d | incursion valid red=%d blue=%d | cp=%d blocked=%d", total, red, blue, incR, incB, cp, blocked);
	PF2NB_Log("gametype=%d koth=%d ad=%d hunted/vip=%d | grenades thrown=%d bot kills=%d bot deaths=%d",
		TFGameRules()->GetGameType(), (int)TFGameRules()->IsInKothMode(), (int)TFGameRules()->IsAttackDefenseMode(),
		(int)TFGameRules()->IsHuntedOrVIPMap(), s_grenadesThrown, s_botKills, s_botDeaths);
	int rooms = 0, inside = 0;
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "func_respawnroom"); e; e = gEntList.FindEntityByClassname(e, "func_respawnroom"))
	{
		++rooms;
		CFuncRespawnRoom *r = static_cast<CFuncRespawnRoom *>(e);
		if (r->PointIsWithin(e->WorldSpaceCenter()))
			++inside;
	}
	int spawns = 0, spawnsInRoom = 0;
	for (CBaseEntity *sp = gEntList.FindEntityByClassname(NULL, "info_player_teamspawn"); sp; sp = gEntList.FindEntityByClassname(sp, "info_player_teamspawn"))
	{
		++spawns;
		for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "func_respawnroom"); e; e = gEntList.FindEntityByClassname(e, "func_respawnroom"))
			if (static_cast<CFuncRespawnRoom *>(e)->PointIsWithin(sp->GetAbsOrigin()))
			{
				++spawnsInRoom;
				break;
			}
	}
	PF2NB_Log("func_respawnroom=%d (centre-inside %d) | info_player_teamspawn=%d (inside a room %d)", rooms, inside, spawns, spawnsInRoom);
	// FFNB: FF team spawns as the nav decoration sees them
	int perTeam[TF_TEAM_COUNT] = {};
	for (int i = 0; i < ITFTeamSpawnAutoList::AutoList().Count(); ++i)
	{
		int t = static_cast<CTFTeamSpawn *>(ITFTeamSpawnAutoList::AutoList()[i])->GetTeamNumber();
		perTeam[(t >= 0 && t < TF_TEAM_COUNT) ? t : 0]++;
	}
	PF2NB_Log("info_ff_teamspawn: %d (none/unknown %d, blue %d, red %d, yellow %d, green %d)", ITFTeamSpawnAutoList::AutoList().Count(),
		perTeam[0] + perTeam[1], perTeam[TF_TEAM_BLUE], perTeam[TF_TEAM_RED], perTeam[TF_TEAM_YELLOW], perTeam[TF_TEAM_GREEN]);
}

CON_COMMAND(ffnb_trace, "ffnb_trace <entindex> - sweep a player hull from the bot toward its next path goal and report the blocker")
{
	if (args.ArgC() < 2)
		return;
	int idx = atoi(args[1]);
	CTFBot *bot = (idx >= 1 && idx <= PF2NB_MAXPLAYERS) ? s_bots[idx] : NULL;
	const PathFollower *path = bot ? bot->GetCurrentPath() : NULL;
	const Path::Segment *goal = path && path->IsValid() ? path->GetCurrentGoal() : NULL;
	if (!goal)
	{
		PF2NB_Log("no bot/path");
		return;
	}
	Vector from = bot->GetAbsOrigin() + Vector(0, 0, 18);
	Vector to = goal->pos + Vector(0, 0, 18);
	trace_t tr;
	UTIL_TraceHull(from, to, Vector(-24, -24, 0), Vector(24, 24, 64), MASK_PLAYERSOLID, bot, COLLISION_GROUP_PLAYER_MOVEMENT, &tr);
	CBaseEntity *hit = (CBaseEntity *)tr.m_pEnt;
	PF2NB_Log("hull trace frac=%.2f startsolid=%d allsolid=%d hit=%s #%d name=%s contents=%x end=(%.0f %.0f %.0f)", tr.fraction, (int)tr.startsolid,
		(int)tr.allsolid, hit ? hit->GetClassname() : "none", hit ? hit->entindex() : -1, hit ? STRING(hit->GetEntityName()) : "",
		tr.contents, tr.endpos.x, tr.endpos.y, tr.endpos.z);
	UTIL_TraceLine(bot->EyePosition(), goal->pos + Vector(0, 0, 40), MASK_PLAYERSOLID, bot, COLLISION_GROUP_PLAYER_MOVEMENT, &tr);
	hit = (CBaseEntity *)tr.m_pEnt;
	PF2NB_Log("eye line frac=%.2f hit=%s #%d", tr.fraction, hit ? hit->GetClassname() : "none", hit ? hit->entindex() : -1);
}

CON_COMMAND(ffnb_field, "ffnb_field <entindex> <field> [int|float|bool]: read an entity field by name (debugging)")
{
	if (args.ArgC() < 3)
		return;
	CBaseEntity *e = PF2_EntityByIndex(atoi(args[1]));
	if (!e)
	{
		PF2NB_Log("no entity %s", args[1]);
		return;
	}
	const char *type = args.ArgC() > 3 ? args[3] : "int";
	if (!V_stricmp(type, "float"))
	{
		float *f = e->Field<float>(args[2]);
		PF2NB_Log("#%s %s %s = %s (curtime %.2f)", args[1], e->GetClassname(), args[2], f ? UTIL_VarArgs("%.2f", *f) : "<no such field>", gpGlobals->curtime);
	}
	else if (!V_stricmp(type, "bool"))
	{
		bool *b = e->Field<bool>(args[2]);
		PF2NB_Log("#%s %s %s = %s", args[1], e->GetClassname(), args[2], b ? (*b ? "true" : "false") : "<no such field>");
	}
	else
	{
		int *i = e->Field<int>(args[2]);
		PF2NB_Log("#%s %s %s = %s", args[1], e->GetClassname(), args[2], i ? UTIL_VarArgs("%d", *i) : "<no such field>");
	}
}

CON_COMMAND(ffnb_ents,"ffnb_ents <classname-pattern> [near-entindex radius] - list entities with origin/name/state")
{
	if (args.ArgC() < 2)
		return;
	CBaseEntity *nearEnt = args.ArgC() > 2 ? PF2_EntityByIndex(atoi(args[2])) : NULL;
	float radius = args.ArgC() > 3 ? (float)atof(args[3]) : 400.0f;
	int n = 0;
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, args[1]); e && n < 40; e = gEntList.FindEntityByClassname(e, args[1]))
	{
		Vector c = e->WorldSpaceCenter();
		if (nearEnt && (c - nearEnt->GetAbsOrigin()).IsLengthGreaterThan(radius))
			continue;
		Vector mn, mx;
		e->CollisionProp()->WorldSpaceAABB(&mn, &mx);
		int *toggle = e->Field<int>("m_toggle_state");
		bool *locked = e->Field<bool>("m_bLocked");
		bool *disabled = e->Field<bool>("m_bDisabled");
		int *flags = e->Field<int>("m_spawnflags");
		CBaseEntity *filter = e->HandleField("m_hFilter");
		PF2NB_Log("#%d %s '%s' team=%d centre=(%.0f %.0f %.0f) box=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) toggle=%d locked=%d disabled=%d sf=%x solid=%d filter=%s(team %d)",
			e->entindex(), e->GetClassname(), STRING(e->GetEntityName()), e->GetTeamNumber(), c.x, c.y, c.z, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z,
			toggle ? *toggle : -1, locked ? (int)*locked : -1, disabled ? (int)*disabled : -1, flags ? *flags : 0, (int)e->IsSolid(),
			filter ? filter->GetClassname() : "-", filter ? filter->GetTeamNumber() : -1);
		++n;
	}
}

// ---------------------------------------------------------------------------------------------
// One-way doors. Some PF2 maps (e.g. ctf_2fort's spawn exits) have doors whose only trigger is on
// one side. TF2's nav decoration records that a team trigger controls the door but not which side it
// is on, so bots inside can path into a door that never opens for them. When a bot has been stuck
// against a closed door for a moment, the door's nav areas are marked blocked for that team
// (CNavArea::MarkAsBlocked, which TFBot's path cost already honours) for a while, and it repaths.
// ---------------------------------------------------------------------------------------------
ConVar pf2nb_door_block_time("ffnb_door_block_time", "30", FCVAR_NONE, "Seconds a door that would not open stays blocked for a team's pathing");

// Blocked areas are remembered by ID, not pointer: a nav generate (pf2nb_nav_generate / navfix) destroys
// every area, and a stale pointer here crashed the next unblock.
static void UnblockAreaID(unsigned int id, int team)
{
	if (CNavArea *a = TheNavMesh ? TheNavMesh->GetNavAreaByID(id) : NULL)
		a->UnblockArea(team);
}
// areas: blocked nav areas (diagonal doors); conns: (from, to) area-ID pairs avoided via the bad-connection list
struct DoorBlock { EHANDLE door; int team; float expire; CUtlVector<unsigned int> areas; CUtlVector<unsigned int> conns; };
static CUtlVector<DoorBlock *> s_doorBlocks;
static void ExpireConnection(unsigned int from, unsigned int to, int team);   // below, with the bad-connection list

static bool DoorIsClosed(CBaseEntity *door)
{
	int *toggle = door->Field<int>("m_toggle_state");
	int *propState = door->Field<int>("m_eDoorState");
	return (toggle && *toggle == TS_AT_BOTTOM) || (propState && *propState == 0);
}
// a door that keeps failing for a team is blocked for longer each time (30, 60, 120, ... 600 s)
struct DoorStrikes { EHANDLE door; int team; int strikes; };
static CUtlVector<DoorStrikes> s_doorStrikes;
static float DoorBlockSeconds(CBaseEntity *door, int team)
{
	for (DoorStrikes &d : s_doorStrikes)
		if (d.door.Get() == door && d.team == team)
		{
			// (MIN is a macro: no side effects in its arguments - ++ inside it counted up to 4 strikes at once)
			d.strikes = MIN(d.strikes + 1, 5);
			return MIN(600.0f, pf2nb_door_block_time.GetFloat() * (float)(1 << d.strikes));
		}
	DoorStrikes d = { door, team, 0 };
	s_doorStrikes.AddToTail(d);
	return pf2nb_door_block_time.GetFloat();
}
static float s_nextDoorCheck = 0.0f;
// Per bot: the path goal area it has been stuck on and since when. The locomotion stuck timer keeps
// running across a repath, so without this the first connection of the new path (e.g. the real spawn
// exit after a one-way door was blocked) was blamed as a bad connection half a second later.
static unsigned int s_stuckGoal[PF2NB_MAXPLAYERS + 1];
static float s_stuckSince[PF2NB_MAXPLAYERS + 1];

struct CollectAreas
{
	CUtlVector<CNavArea *> *out;
	bool operator()(CNavArea *a) { out->AddToTail(a); return true; }
};

static void ClearDoorBlocks(bool all)
{
	for (int i = s_doorBlocks.Count() - 1; i >= 0; --i)
	{
		DoorBlock *b = s_doorBlocks[i];
		// a block is lifted when it expires, and as soon as the door opens: doors that are only shut for a
		// while (hunted_actually_accurate's blublastdoor stays closed until setup ends) were otherwise avoided
		// for up to minutes after opening (30 -> 60 -> 120 s strikes during setup), so BLU's bodyguards
		// never left spawn, the gate stayed shut and the VIP waited in spawn for the whole round
		CBaseEntity *door = b->door.Get();
		if (!all && gpGlobals->curtime < b->expire && door && DoorIsClosed(door))
			continue;
		if (!all)
		{
			for (unsigned int id : b->areas)
				UnblockAreaID(id, b->team);
			for (int k = 0; k + 1 < b->conns.Count(); k += 2)
				ExpireConnection(b->conns[k], b->conns[k + 1], b->team);
			if (door && pf2nb_debug.GetBool() && gpGlobals->curtime < b->expire)
				PF2NB_Log("door %s '%s' opened; lifted its block for team %d", door->GetClassname(), STRING(door->GetEntityName()), b->team);
		}
		delete b;
		s_doorBlocks.Remove(i);
	}
}

static bool IsDoorBlockedFor(CBaseEntity *door, int team)
{
	for (DoorBlock *b : s_doorBlocks)
		if (b->door.Get() == door && b->team == team)
			return true;
	return false;
}

// Bad mesh connections: a bot stuck on one area-to-area step (not a door) for a while marks that
// connection unusable for its team; CTFBotPathCost skips it so the path goes round. Escalates 60 s ->
// 600 s per repeat; cleared on map change.
ConVar pf2nb_bad_connection_time("ffnb_bad_connection_time", "60", FCVAR_NONE, "Seconds a mesh connection bots got stuck on is avoided (doubles per repeat, max 600; 0 = off)");
struct BadConnection { unsigned int from, to; int team; float expire; int strikes; };
static CUtlVector<BadConnection> s_badConnections;

// Impossible drops. The generator links an area to a much lower one whenever their edges line up, also
// where the drop is not open: hunted_stonehenge_test's spawn ramp (66) is linked west to the room
// floor (13) that runs *under* the ramp, 236 units down. Paths took that "drop" (it is the shortest way
// down), the bot stood at the ramp's edge waiting to fall and never did, so the VIP never left BLU's
// spawn and the bodyguards escorting him stood with him. Each long drop onto an area that runs under
// the upper one is checked once per map with world-only hull traces (doors and brush entities ignored):
// step over the edge, fall, and see whether the fall ends near the lower area. A drop that lands nowhere
// near it is skipped by CTFBotPathCost.
ConVar ffnb_prune_drops("ffnb_prune_drops", "1", FCVAR_NONE, "At map load, drop connections whose fall cannot reach the lower area are not used by bots");
static CUtlVector<uint64> s_impossibleDrops;   // sorted (from << 32 | to)

static inline uint64 DropKey(unsigned int from, unsigned int to) { return ((uint64)from << 32) | to; }

static bool IsImpossibleDrop(const CNavArea *from, const CNavArea *to)
{
	int lo = 0, hi = s_impossibleDrops.Count() - 1;
	uint64 key = DropKey(from->GetID(), to->GetID());
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if (s_impossibleDrops[mid] == key)
			return true;
		if (s_impossibleDrops[mid] < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

// can a player standing at 'edge' (on 'from', at its rim toward 'dir') step off and fall into 'to'?
static bool DropLandsIn(const CNavArea *from, const CNavArea *to, NavDirType dir, Vector edge)
{
	CTraceFilterWorldOnly world;
	const Vector mins(-8, -8, 0), maxs(8, 8, 40);
	edge.z = from->GetZ(edge);
	Vector back = edge, over = edge;
	AddDirectionVector(&back, dir, -16.0f);
	AddDirectionVector(&over, dir, 16.0f + 1.0f);
	back.z = from->GetZ(back) + StepHeight;
	over.z = back.z;
	trace_t tr;
	UTIL_TraceHull(back, over, mins, maxs, MASK_PLAYERSOLID_BRUSHONLY, &world, &tr);
	if (tr.startsolid || tr.fraction < 1.0f)
		return false;   // a wall or railing at the rim
	float zTo = to->GetZ(over);
	UTIL_TraceHull(over, Vector(over.x, over.y, zTo - 8.0f), mins, maxs, MASK_PLAYERSOLID_BRUSHONLY, &world, &tr);
	return tr.endpos.z <= zTo + StepHeight;   // landed on (or near) the lower area, not on something above it
}

// Walk a connection with a player hull (world geometry only; doors and props open or move): from
// 'from' at its rim, across the portal into 'to'. Covers what the generator links but a player cannot do:
// railings and thin walls between two floors, ledges too high to jump, low ceilings over a step.
static bool s_walkDebug = false;
static bool WalkCrosses(const CNavArea *from, const CNavArea *to, NavDirType dir, Vector edge);
CON_COMMAND(ffnb_walk, "ffnb_walk <from-area> <to-area>: walk one nav connection with a player hull and print the traces")
{
	if (args.ArgC() < 3 || !TheNavMesh)
		return;
	CNavArea *a = TheNavMesh->GetNavAreaByID(atoi(args[1])), *b = TheNavMesh->GetNavAreaByID(atoi(args[2]));
	if (!a || !b)
		return;
	for (int d = 0; d < NUM_DIRECTIONS; ++d)
		if (a->IsConnected(b, (NavDirType)d))
		{
			Vector c;
			float half;
			a->ComputePortal(b, (NavDirType)d, &c, &half);
			PF2NB_Log("connection %u -> %u dir %d portal (%.0f %.0f %.0f) half %.0f zFrom %.0f zTo %.0f", a->GetID(), b->GetID(), d, c.x, c.y, c.z, half, a->GetZ(c), b->GetZ(c));
			s_walkDebug = true;
			for (int s = -1; s <= 1; ++s)
			{
				Vector p = c;
				float off = s * MAX(0.0f, half - 16.0f);
				if (d == NORTH || d == SOUTH) p.x += off; else p.y += off;
				PF2NB_Log("  sample %d: %s", s, WalkCrosses(a, b, (NavDirType)d, p) ? "crosses" : "BLOCKED");
			}
			s_walkDebug = false;
		}
}

static bool WalkCrosses(const CNavArea *from, const CNavArea *to, NavDirType dir, Vector edge)
{
	CTraceFilterWorldOnly world;
	edge.z = from->GetZ(edge);
	Vector back = edge, over = edge;
	// a railing or thin wall the generator linked across sits on the portal; a short crossing finds it and
	// keeps the hull clear of walls running alongside (areas extend half a sample step past valid spots)
	AddDirectionVector(&back, dir, -12.0f);
	AddDirectionVector(&over, dir, 12.0f);
	float zFrom = from->GetZ(back), zTo = to->GetZ(over);
	float dz = zTo - zFrom;
	if (dz > JumpCrouchHeight + 2.0f)
		return false;   // no jump reaches it (ladders are separate connections)
	// a narrow hull (as for drops): walls running alongside a narrow portal must not count, railings and
	// thin walls across it still do
	const Vector mins(-8, -8, 0), maxs(8, 8, HumanCrouchHeight - StepHeight);
	trace_t tr;
	float startZ = zFrom + StepHeight;
	if (dz > StepHeight)
	{
		// a jump: room to get up from our floor to a step above the target floor
		float topZ = zTo + StepHeight;
		UTIL_TraceHull(Vector(back.x, back.y, startZ), Vector(back.x, back.y, topZ), mins, maxs, MASK_PLAYERSOLID_BRUSHONLY, &world, &tr);
		if (s_walkDebug) PF2NB_Log("    jump trace (%.0f %.0f %.0f)->z %.0f: startsolid %d frac %.2f ent #%d", back.x, back.y, startZ, topZ, tr.startsolid, tr.fraction, tr.m_pEnt ? ((CBaseEntity *)tr.m_pEnt)->entindex() : -1);
		if (tr.startsolid || tr.fraction < 1.0f)
			return false;
		startZ = topZ;
	}
	// across, a step above the higher floor (follows rising slopes and stairs; going down we fall after the rim)
	UTIL_TraceHull(Vector(back.x, back.y, startZ), Vector(over.x, over.y, MAX(zTo, zFrom) + StepHeight), mins, maxs,
		MASK_PLAYERSOLID_BRUSHONLY, &world, &tr);
	if (s_walkDebug) PF2NB_Log("    walk trace (%.0f %.0f %.0f)->(%.0f %.0f): startsolid %d frac %.2f normal (%.2f %.2f %.2f)", back.x, back.y, startZ, over.x, over.y, tr.startsolid, tr.fraction, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z);
	return !tr.startsolid && tr.fraction >= 1.0f;
}

ConVar ffnb_prune_walls("ffnb_prune_walls", "1", FCVAR_NONE, "At map load, connections a player hull cannot walk or jump across (railings, thin walls, ledges too high) are not used by bots");

static void PruneImpossibleDrops()
{
	s_impossibleDrops.RemoveAll();
	if (!ffnb_prune_drops.GetBool() || !TheNavMesh)
		return;
	int checked = 0, checkedWalls = 0, walls = 0;
	FOR_EACH_VEC(TheNavAreas, it)
	{
		CNavArea *a = TheNavAreas[it];
		for (int d = 0; d < NUM_DIRECTIONS; ++d)
		{
			const NavConnectVector *adj = a->GetAdjacentAreas((NavDirType)d);
			FOR_EACH_VEC(*adj, k)
			{
				CNavArea *b = (*adj)[k].area;
				if (!b)
					continue;
				Vector centre;
				float half;
				a->ComputePortal(b, (NavDirType)d, &centre, &half);
				if (a->GetZ(centre) - b->GetZ(centre) <= JumpCrouchHeight)
				{
					// walkable, a step, a jump up or a short hop down: walk it with a hull
					if (!ffnb_prune_walls.GetBool() || half < 1.0f || a->IsUnderwater() || b->IsUnderwater())
						continue;   // swimming crosses heights a walking hull cannot
					++checkedWalls;
					bool ok = false;
					for (int s = -1; s <= 1 && !ok; ++s)
					{
						Vector p = centre;
						float off = s * MAX(0.0f, half - 16.0f);
						if (d == NORTH || d == SOUTH)
							p.x += off;
						else
							p.y += off;
						ok = WalkCrosses(a, b, (NavDirType)d, p);
					}
					if (!ok)
					{
						s_impossibleDrops.AddToTail(DropKey(a->GetID(), b->GetID()));
						++walls;
					}
					continue;
				}
				// only a lower area that runs underneath this one (the generator linked a floor to the one
				// below it); drops off a ledge across a gap - common on displacement terrain, where the traces
				// below are not exact - are left alone
				const Vector &an = a->GetCorner(NORTH_WEST), &as = a->GetCorner(SOUTH_EAST);
				const Vector &bn = b->GetCorner(NORTH_WEST), &bs = b->GetCorner(SOUTH_EAST);
				if (MIN(as.x, bs.x) - MAX(an.x, bn.x) < 16.0f || MIN(as.y, bs.y) - MAX(an.y, bn.y) < 16.0f)
					continue;
				++checked;
				// the portal's middle and both ends (inset by a half hull); one open spot is enough
				bool ok = false;
				for (int s = -1; s <= 1 && !ok; ++s)
				{
					Vector p = centre;
					float off = s * MAX(0.0f, half - 16.0f);
					if (d == NORTH || d == SOUTH)
						p.x += off;
					else
						p.y += off;
					ok = DropLandsIn(a, b, (NavDirType)d, p);
				}
				if (!ok)
					s_impossibleDrops.AddToTail(DropKey(a->GetID(), b->GetID()));
			}
		}
	}
	s_impossibleDrops.Sort([](const uint64 *l, const uint64 *r) { return *l < *r ? -1 : (*l > *r ? 1 : 0); });
	PF2NB_Log("nav: %d drops onto a floor below checked, %d walk/jump connections checked; %d impossible (%d walls/ledges), bots will not use them; ffnb_drops lists them",
		checked, checkedWalls, s_impossibleDrops.Count(), walls);
}
void PF2NB_PruneDrops() { PruneImpossibleDrops(); }
CON_COMMAND(ffnb_drops, "Re-check the nav mesh's long drops and list the ones bots will not use")
{
	if (!TheNavMesh || !TheNavMesh->IsLoaded())
		return;
	PF2_InvalidateEntityCache();
	PruneImpossibleDrops();
	for (uint64 k : s_impossibleDrops)
		PF2NB_Log("   impossible drop %u -> %u", (unsigned int)(k >> 32), (unsigned int)(k & 0xffffffffu));
}
void PF2NB_ForgetDrops() { s_impossibleDrops.RemoveAll(); }

bool PF2NB_IsBadConnection(const CNavArea *from, const CNavArea *to, int team)
{
	if (!from || !to)
		return false;
	if (s_impossibleDrops.Count() && IsImpossibleDrop(from, to))
		return true;
	if (s_badConnections.Count() == 0)
		return false;
	unsigned int f = from->GetID(), t = to->GetID();
	for (const BadConnection &b : s_badConnections)
		if (b.from == f && b.to == t && b.team == team && gpGlobals->curtime < b.expire)
			return true;
	return false;
}

// Avoiding a connection must only ever reroute: if the bot's goal is no longer reachable from its area
// with the current avoid list, the connection was the last way out (e.g. two bots wedged in BLU's spawn
// corridor on ctf_classic_2fort_pf2 marked the real exit after the one-way door) and the mark is undone.
static bool GoalStillReachable(CTFBot *bot, CNavArea *from, const Vector &goal)
{
	CNavArea *goalArea = TheNavMesh->GetNearestNavArea(goal, false, 500.0f, false, false);
	if (!from || !goalArea || goalArea == from)
		return true;
	CTFBotPathCost cost(bot, FASTEST_ROUTE);
	return NavAreaBuildPath(from, goalArea, &goal, cost, NULL, 0.0f, bot->GetTeamNumber());
}

static void NoteBadConnection(CTFBot *bot, CNavArea *from, CNavArea *to, PathFollower *path)
{
	int team = bot->GetTeamNumber();
	Vector end = path->GetEndPosition();
	float base = pf2nb_bad_connection_time.GetFloat();
	if (base <= 0.0f)
		return;
	unsigned int f = from->GetID(), t = to->GetID();
	for (BadConnection &b : s_badConnections)
	{
		if (b.from == f && b.to == t && b.team == team)
		{
			if (gpGlobals->curtime < b.expire)
				return;   // already avoided; the bot is stuck for another reason
			float old = b.expire;
			b.expire = gpGlobals->curtime + MIN(600.0f, base * (float)(1 << MIN(b.strikes + 1, 4)));
			if (!GoalStillReachable(bot, from, end))
			{
				b.expire = old;
				return;
			}
			b.strikes = MIN(b.strikes + 1, 4);
			path->Invalidate();
			return;
		}
	}
	// a bot snagged inside an area (props, a lip) gets stuck on every way out of it: once two exits of
	// the same area are being avoided, the problem is not the connection - don't cut the area off
	int fromCount = 0;
	for (const BadConnection &o : s_badConnections)
		if (o.from == f && o.team == team && gpGlobals->curtime < o.expire)
			++fromCount;
	if (fromCount >= 2)
		return;
	BadConnection b = { f, t, team, gpGlobals->curtime + base, 0 };
	s_badConnections.AddToTail(b);
	if (!GoalStillReachable(bot, from, end))
	{
		s_badConnections.Remove(s_badConnections.Count() - 1);
		if (pf2nb_debug.GetBool())
			PF2NB_Log("team %d stuck on nav connection %u -> %u, but it is the only way to the goal; not avoiding it", team, f, t);
		return;
	}
	path->Invalidate();
	if (pf2nb_debug.GetBool())
		PF2NB_Log("team %d stuck on nav connection %u -> %u; avoiding it for %.0fs", team, f, t, base);
}

// a one-way door's crossing, avoided by a team until 'expire' (see CheckStuckAtDoors)
static void AddDoorConnection(unsigned int from, unsigned int to, DoorBlock *block)
{
	block->conns.AddToTail(from);
	block->conns.AddToTail(to);
	for (BadConnection &b : s_badConnections)
		if (b.from == from && b.to == to && b.team == block->team)
		{
			b.expire = MAX(b.expire, block->expire);
			return;
		}
	BadConnection b = { from, to, block->team, block->expire, 0 };
	s_badConnections.AddToTail(b);
}

// the door that made this connection bad has opened: usable again right away
static void ExpireConnection(unsigned int from, unsigned int to, int team)
{
	for (BadConnection &b : s_badConnections)
		if (b.from == from && b.to == to && b.team == team)
			b.expire = MIN(b.expire, gpGlobals->curtime);
}

static void CheckStuckAtDoors()
{
	if (gpGlobals->curtime < s_nextDoorCheck && gpGlobals->curtime > s_nextDoorCheck - 1.0f)
		return;
	s_nextDoorCheck = gpGlobals->curtime + 0.5f;
	ClearDoorBlocks(false);
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFBot *bot = s_bots[i];
		if (!bot || !bot->IsAlive())
		{
			s_stuckGoal[i] = 0;
			continue;
		}
		ILocomotion *loc = bot->GetLocomotionInterface();
		const PathFollower *path = bot->GetCurrentPath();
		const Path::Segment *goal = path && path->IsValid() ? path->GetCurrentGoal() : NULL;
		if (!goal || !loc->IsStuck())
		{
			s_stuckGoal[i] = 0;
			continue;
		}
		unsigned int goalID = goal->area ? goal->area->GetID() + 1 : 1;
		if (s_stuckGoal[i] != goalID)
		{
			s_stuckGoal[i] = goalID;
			s_stuckSince[i] = gpGlobals->curtime;
		}
		if (loc->GetStuckDuration() < 1.5f)
			continue;
		trace_t tr;
		UTIL_TraceHull(bot->GetAbsOrigin() + Vector(0, 0, 18), goal->pos + Vector(0, 0, 18), Vector(-24, -24, 0), Vector(24, 24, 64),
			bot->GetBodyInterface()->GetSolidMask(), bot, COLLISION_GROUP_PLAYER_MOVEMENT, &tr);
		CBaseEntity *door = (CBaseEntity *)tr.m_pEnt;
		if (!door || !(door->ClassMatches("func_door*") || door->ClassMatches("prop_door*")))
		{
			// not a door: after a few seconds, the mesh connection itself is suspect
			if (loc->GetStuckDuration() >= 3.0f && gpGlobals->curtime - s_stuckSince[i] >= 3.0f && goal->area && bot->GetLastKnownArea() && goal->area != bot->GetLastKnownArea())
				NoteBadConnection(bot, bot->GetLastKnownArea(), goal->area, const_cast<PathFollower *>(path));
			continue;
		}
		if (!DoorIsClosed(door) || IsDoorBlockedFor(door, bot->GetTeamNumber()))
			continue;
		DoorBlock *b = new DoorBlock;
		b->door = door;
		b->team = bot->GetTeamNumber();
		float seconds = DoorBlockSeconds(door, b->team);
		b->expire = gpGlobals->curtime + seconds;
		Extent ext;
		door->CollisionProp()->WorldSpaceAABB(&ext.lo, &ext.hi);
		ext.lo -= Vector(32, 32, 64);
		ext.hi += Vector(32, 32, 32);
		CUtlVector<CNavArea *> found;
		CollectAreas c = { &found };
		TheNavMesh->ForAllAreasOverlappingExtent(c, ext);
		// One-way doors are blocked in one direction only: the nav connections near the door that lead from
		// the stuck bot's side to the far side are marked bad for the team (CTFBotPathCost skips them), so
		// teammates on the far side still path through the door. Blocking the door's areas instead cut
		// spawns off: on ctf_classic_2fort_pf2 the padded box reached BLU's spawn-floor area (cutting the
		// whole spawn off - bots ran a trivial path into the corner nearest the enemy flag), and a bot
		// snagged outside an exit door blocked that exit for everyone inside.
		Vector dlo, dhi;
		door->CollisionProp()->WorldSpaceAABB(&dlo, &dhi);
		float sx = dhi.x - dlo.x, sy = dhi.y - dlo.y;
		int axis = (sx * 2.0f < sy) ? 0 : (sy * 2.0f < sx) ? 1 : -1;   // the door's thin axis (-1: diagonal/square)
		int marked = 0;
		if (axis >= 0)
		{
			float mid = 0.5f * (dlo[axis] + dhi[axis]);
			float side = bot->GetAbsOrigin()[axis] - mid;
			CNavArea *mine = bot->GetLastKnownArea();
			for (CNavArea *a : found)
			{
				if (a != mine && (a->GetCenter()[axis] - mid) * side <= 0.0f)
					continue;   // not on the bot's side
				for (int d = 0; d < NUM_DIRECTIONS; ++d)
				{
					const NavConnectVector *adj = a->GetAdjacentAreas((NavDirType)d);
					FOR_EACH_VEC(*adj, k)
					{
						CNavArea *n = (*adj)[k].area;
						if (!n || n == mine || (n->GetCenter()[axis] - mid) * side > 0.0f || found.Find(n) < 0)
							continue;   // same side, or not at this door
						AddDoorConnection(a->GetID(), n->GetID(), b);
						++marked;
					}
				}
			}
		}
		else
		{
			for (CNavArea *a : found)
			{
				a->MarkAsBlocked(b->team, door, false);
				b->areas.AddToTail(a->GetID());
			}
		}
		if (marked == 0 && b->areas.Count() == 0 && goal->area && bot->GetLastKnownArea() && goal->area != bot->GetLastKnownArea())
		{
			AddDoorConnection(bot->GetLastKnownArea()->GetID(), goal->area->GetID(), b);
			++marked;
		}
		s_doorBlocks.AddToTail(b);
		const_cast<PathFollower *>(path)->Invalidate();
		if (pf2nb_debug.GetBool())
			PF2NB_Log("door %s '%s' did not open for team %d; blocked %d areas, %d connections toward it for %.0fs", door->GetClassname(),
				STRING(door->GetEntityName()), b->team, b->areas.Count(), marked, seconds);
	}
}
void PF2NB_DoorsFrame() { CheckStuckAtDoors(); }
void PF2NB_DoorsReset() { ClearDoorBlocks(true); s_doorStrikes.RemoveAll(); s_badConnections.RemoveAll(); V_memset(s_stuckGoal, 0, sizeof(s_stuckGoal)); }

// ---------------------------------------------------------------------------------------------
// Hazards. TF2's offline nav generator does not know about trigger_hurt, so meshes can lead bots into
// traps and kill volumes (e.g. hunted_accurate's "cage of torment"). Active damaging triggers have
// their nav areas blocked for the teams they hurt, refreshed every few seconds.
// ---------------------------------------------------------------------------------------------
ConVar tf_bot_pf2_avoid_hurt_triggers("ff_bot_avoid_hurt_triggers", "1", FCVAR_NONE, "Keep bots out of active trigger_hurt volumes");
struct HazardBlock { unsigned int area; int team; };   // area ID (see UnblockAreaID)
static CUtlVector<HazardBlock> s_hazardBlocks;
static float s_nextHazardScan = 0.0f;

static void ClearHazards(bool unblock)
{
	if (unblock)
		for (HazardBlock &h : s_hazardBlocks)
			UnblockAreaID(h.area, h.team);
	s_hazardBlocks.RemoveAll();
}

static void BlockHazardFor(const Extent &ext, int team)
{
	CUtlVector<CNavArea *> areas;
	CollectAreas c = { &areas };
	TheNavMesh->ForAllAreasOverlappingExtent(c, ext);
	for (CNavArea *a : areas)
	{
		a->MarkAsBlocked(team, NULL, false);
		HazardBlock h = { a->GetID(), team };
		s_hazardBlocks.AddToTail(h);
	}
}

static void ScanHazards()
{
	if (gpGlobals->curtime < s_nextHazardScan && gpGlobals->curtime > s_nextHazardScan - 6.0f)
		return;
	s_nextHazardScan = gpGlobals->curtime + 5.0f;
	ClearHazards(true);
	if (!tf_bot_pf2_avoid_hurt_triggers.GetBool())
		return;
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "trigger_hurt*"); e; e = gEntList.FindEntityByClassname(e, "trigger_hurt*"))
	{
		bool *disabled = e->Field<bool>("m_bDisabled");
		float *damage = e->Field<float>("m_flDamage");
		if ((disabled && *disabled) || (damage && *damage <= 0.0f))
			continue;
		Extent ext;
		e->CollisionProp()->WorldSpaceAABB(&ext.lo, &ext.hi);
		// map-wide kill volumes (hunted maps kill the losing team at round end: Stonehenge's
		// trigger_hurt_civ_failed_to_ascend covers the whole map) cannot be avoided; blocking them blocked
		// every area for that team, so its bots stood still
		if ((ext.hi - ext.lo).AsVector2D().IsLengthGreaterThan(6000.0f))
			continue;
		ext.lo.z -= HalfHumanHeight;
		ext.hi.z -= 4.0f;   // floors resting exactly on a hurt volume's top face (Stonehenge's water) stay usable
		CBaseEntity *filter = e->HandleField("m_hFilter");
		if (!filter)
		{
			// FFNB: a Lua-named trigger_hurt filters through its Lua class: not_<team>_trigger (base_shutdown
			// lasers, spawn "slayers") hurts everyone but <team>. Block it for every active team it hurts.
			extern int FF_CensusSpareTeam(int entindex);
			int spare = FF_CensusSpareTeam(e->entindex());
			for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
				if (t != spare && FF_TeamIsActive(t))
					BlockHazardFor(ext, t);
		}
		else if (filter->ClassMatches("filter_activator_tfteam"))
		{
			bool *neg = filter->Field<bool>("m_bNegated");
			int t = filter->GetTeamNumber();
			int hurt = (neg && *neg) ? GetEnemyTeam(t) : t;
			if (hurt == TF_TEAM_RED || hurt == TF_TEAM_BLUE)
				BlockHazardFor(ext, hurt);
		}
		// class filters (hunted spawns) are left to the spawn-death class learning
	}
}
void PF2NB_HazardsFrame() { ScanHazards(); }
void PF2NB_HazardsReset() { ClearHazards(false); s_nextHazardScan = 0.0f; }
