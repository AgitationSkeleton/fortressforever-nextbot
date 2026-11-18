// ff-nextbot: fake-client creation and per-frame drive for Fortress Forever.
//
// FF's own bot (ff_bot_temp.cpp) never executes its usercmd, so nothing in the game drives a fake
// client. We create bots with the engine's IBotManager::CreateBot (a plain CFFPlayer), join them with
// FF's "team <color>" / "class <name>" client commands, and every frame call
// IBotController::RunPlayerMove. That runs CBasePlayer::PlayerRunCommand, where our vtable hook lets
// the bot's brain rewrite the usercmd (same hook point as pf2-nextbot).
#include <stdio.h>
#include "plugin.h"
#include <tier0/platform.h>
#include "botdriver.h"
#include "botmanager.h"
#include <eiface.h>
#include <iserverunknown.h>
#include <iservernetworkable.h>
#include <convar.h>
#include <edict.h>
#include <iplayerinfo.h>
#include "game/pf2_props.h"
#include "game/pf2_offsets.h"
#include "game/vfunc.h"
#include <utlvector.h>
#include <vstdlib/random.h>

IBotManager *botmanager = NULL;
void FF_NoteDesiredClass(int entindex, const char *className);   // shim/pf2_tf.cpp
static ConVar ffnb_drive_debug("ffnb_drive_debug", "0", FCVAR_NONE, "Log the per-frame bot drive state every 5 s");
static ConVar ffnb_isbot_vtidx("ffnb_isbot_vtidx", "450", FCVAR_NONE, "CBasePlayer::IsBot vtable slot (diagnostics only)");

// FFNB: FF's IBotController (FF public/game/server/iplayerinfo.h) has PostClientMessagesSent()
// before IsEFlagSet(), one slot more than the hl2sdk header. Calling RunPlayerMove through the
// stock header lands on IsEFlagSet. Mirror FF's layout exactly.
class IFFBotController
{
public:
	virtual void SetAbsOrigin(Vector &vec) = 0;
	virtual void SetAbsAngles(QAngle &ang) = 0;
	virtual void SetLocalOrigin(const Vector &origin) = 0;
	virtual const Vector GetLocalOrigin(void) = 0;
	virtual void SetLocalAngles(const QAngle &angles) = 0;
	virtual const QAngle GetLocalAngles(void) = 0;
	virtual void RemoveAllItems(bool removeSuit) = 0;
	virtual void SetActiveWeapon(const char *WeaponName) = 0;
	virtual void PostClientMessagesSent(void) = 0;
	virtual bool IsEFlagSet(int nEFlagMask) = 0;
	virtual void RunPlayerMove(CBotCmd *ucmd) = 0;
};

namespace pf2 {

void *RawEntity(int idx)
{
	edict_t *ed = engine->PEntityOfEntIndex(idx);
	if (!ed || ed->IsFree())
		return NULL;
	IServerUnknown *u = ed->GetUnknown();
	return u ? (void *)u->GetBaseEntity() : NULL;
}

static void BotClientCommand(edict_t *ed, const char *line)
{
	CCommand args;
	if (args.Tokenize(line))
		gameclients->ClientCommand(ed, args);
}

struct SpawnReq { char team[16]; char cls[24]; char name[64]; SpawnCallback cb; void *ctx; };
static CUtlVector<SpawnReq> s_spawnQueue;

void Puppet_Request(const char *team, const char *cls, const char *name, SpawnCallback cb, void *ctx)
{
	SpawnReq r;
	V_strncpy(r.team, team, sizeof(r.team));
	V_strncpy(r.cls, cls, sizeof(r.cls));
	V_strncpy(r.name, name, sizeof(r.name));
	r.cb = cb;
	r.ctx = ctx;
	s_spawnQueue.AddToTail(r);
}

int Puppet_Pending() { return s_spawnQueue.Count(); }

// FF's CBasePlayer::Spawn clears m_fFlags, dropping FL_FAKECLIENT, and CPlayerInfo::RunPlayerMove
// only runs `if (IsBot())`. FF's own bot loop re-adds the flag every frame; so do we.
static void KeepBotFlags(void *raw)
{
	int *flags = PF2_SENDPROP_PTR(int, raw, "m_fFlags");
	if (flags)
		*flags |= FL_CLIENT | FL_FAKECLIENT;
}

void Puppet_Frame()
{
	if (!s_spawnQueue.Count())
		return;
	SpawnReq r = s_spawnQueue[0];
	s_spawnQueue.Remove(0);
	edict_t *ed = botmanager ? botmanager->CreateBot(r.name) : NULL;
	int idx = ed ? engine->IndexOfEdict(ed) : 0;
	if (!idx)
	{
		PF2NB_Log("spawn of '%s' failed (CreateBot returned no edict)", r.name);
	}
	else
	{
		KeepBotFlags(RawEntity(idx));
		char line[96];
		V_snprintf(line, sizeof(line), "team %s", r.team);
		BotClientCommand(ed, line);
		V_snprintf(line, sizeof(line), "class %s", r.cls);
		BotClientCommand(ed, line);
		::FF_NoteDesiredClass(idx, r.cls);
	}
	if (r.cb)
		r.cb(idx, r.ctx);
}

// Called once per server frame, after GameFrame: run one usercmd for every driven bot.
void Puppet_Drive()
{
	if (!botmanager)
		return;
	for (int i = 1; i <= gpGlobals->maxClients && i <= PF2NB_MAXPLAYERS; ++i)
	{
		if (!Drivers_Get(i))
			continue;
		edict_t *ed = engine->PEntityOfEntIndex(i);
		void *raw = RawEntity(i);
		if (!ed || !raw)
			continue;
		KeepBotFlags(raw);
		IFFBotController *ctrl = (IFFBotController *)botmanager->GetBotController(ed);
		static float s_nextDiag = 0.0f;
		if (ffnb_drive_debug.GetBool() && gpGlobals->curtime >= s_nextDiag)
		{
			s_nextDiag = gpGlobals->curtime + 5.0f;
			int *flags = PF2_SENDPROP_PTR(int, raw, "m_fFlags");
			bool isBot = VCall<bool>(raw, ffnb_isbot_vtidx.GetInt());
			PF2NB_Log("drive #%d: ctrl=%p flags=0x%x IsBot()=%d hookCalls=%d", i, ctrl, flags ? *flags : -1, isBot ? 1 : 0, Drivers_HookCalls(i));
		}
		if (!ctrl)
			continue;
		CBotCmd cmd;
		cmd.command_number = gpGlobals->tickcount;
		cmd.tick_count = gpGlobals->tickcount;
		cmd.random_seed = RandomInt(0, 0x7fffffff);
		ctrl->RunPlayerMove(&cmd);   // -> PlayerRunCommand hook -> IPF2BotDriver::OnRunCommand
		ctrl->PostClientMessagesSent();   // as FF's omnibot bridge does after each move
	}
}

} // namespace pf2
