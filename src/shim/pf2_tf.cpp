// pf2-nextbot game layer: TF/PF2 proxy implementations.
#include "cbase.h"
#include "tf_nav_mesh.h"
#include "game/vfunc.h"
#include "game/pf2_offsets.h"
#include <dt_send.h>
#include <server_class.h>

using namespace pf2;

#include "game/ff_census.h"
#include "game/ff_weapons.h"   // FFNB: weapon ids/slots come from the FF weapon table, not vtable calls

// ---------------------------------------------------------------------------------------------
// Sendtable array helper: offset of element i of a networked array (handles int/bool strides)
// ---------------------------------------------------------------------------------------------
static SendProp *FindSendPropDeep(SendTable *t, const char *name, int base, int *outOff)
{
	for (int i = 0; i < t->GetNumProps(); ++i)
	{
		SendProp *p = t->GetProp(i);
		if (p->GetName() && !V_strcmp(p->GetName(), name))
		{
			*outOff = base + p->GetOffset();
			return p;
		}
		if (p->GetType() == DPT_DataTable && p->GetDataTable())
			if (SendProp *r = FindSendPropDeep(p->GetDataTable(), name, base + p->GetOffset(), outOff))
				return r;
	}
	return NULL;
}

static int SendArrayElementOffset(const void *raw, const char *name, int index)
{
	if (!raw)
		return -1;
	ServerClass *sc = VCall<ServerClass *>(raw, vt::GetServerClass);
	int off = 0;
	SendProp *p = sc ? FindSendPropDeep(sc->m_pTable, name, 0, &off) : NULL;
	if (!p)
		return -1;
	if (p->GetType() == DPT_DataTable && p->GetDataTable())
	{
		SendTable *t = p->GetDataTable();
		if (index < 0 || index >= t->GetNumProps())
			return -1;
		return off + t->GetProp(index)->GetOffset();
	}
	return off;   // not an array table
}

template <typename T>
static T SendArrayGet(const CBaseEntity *e, const char *name, int index, T def)
{
	void *raw = e ? e->GetRaw() : NULL;
	int o = SendArrayElementOffset(raw, name, index);
	return o < 0 ? def : *(T *)((char *)raw + o);
}

// ---------------------------------------------------------------------------------------------
// Proxy factory
// ---------------------------------------------------------------------------------------------
static bool StartsWith(const char *s, const char *p) { return !V_strnicmp(s, p, strlen(p)); }
static bool EndsWith(const char *s, const char *p) { size_t a = strlen(s), b = strlen(p); return a >= b && !V_stricmp(s + a - b, p); }

class CTFWeaponProxy : public CTFWeaponBase {};

CBaseEntity *PF2_CreateProxyForClass(const char *cls, int index)
{
	if (!cls) cls = "";
	// FFNB: Lua objectives share generic classnames; the census says what they are
	if (const ff::CensusObj *o = ff::Census_Get(index))
	{
		if (o->kind == ff::OBJ_FLAG || o->kind == ff::OBJ_BALL)
			return new CCaptureFlag;
		if (o->kind == ff::OBJ_CAP)
			return new CCaptureZone;
	}
	if (!V_strcmp(cls, "player")) return new CTFPlayer;
	if (StartsWith(cls, "tf_projectile_pipe") || !V_strcmp(cls, "tf_weapon_grenade_pipebomb_projectile")) return new CTFGrenadePipebombProjectile;
	if (!V_strcmp(cls, "info_ff_teamspawn")) return new CTFTeamSpawn;   // FFNB
	if (!V_strcmp(cls, "trigger_multiple")) return new CTriggerMultiple;
	// FFNB: Fortress Forever weapons, pipebombs and buildings
	if (!V_strcmp(cls, "ff_projectile_pl")) return new CTFGrenadePipebombProjectile;
	if (StartsWith(cls, "ff_weapon_"))
	{
		if (!V_strcmp(cls, "ff_weapon_sniperrifle")) return new CTFSniperRifle;
		if (!V_strcmp(cls, "ff_weapon_pipelauncher")) return new CTFPipebombLauncher;
		if (!V_strcmp(cls, "ff_weapon_flamethrower")) return new CTFFlameThrower;
		return new CTFWeaponProxy;
	}
	if (!V_stricmp(cls, "FF_SentryGun")) return new CObjectSentrygun;
	if (!V_stricmp(cls, "FF_Dispenser")) return new CObjectDispenser;
	if (!V_stricmp(cls, "FF_Detpack") || !V_stricmp(cls, "FF_ManCannon")) return new CBaseObject;
	if (StartsWith(cls, "tf_weapon_") && !EndsWith(cls, "_projectile") && !StartsWith(cls, "tf_weapon_grenade_mirv_bomb"))
	{
		if (!V_strcmp(cls, "tf_weapon_medigun")) return new CWeaponMedigun;
		if (!V_strcmp(cls, "tf_weapon_sniperrifle")) return new CTFSniperRifle;
		if (!V_strcmp(cls, "tf_weapon_pipebomblauncher")) return new CTFPipebombLauncher;
		if (!V_strcmp(cls, "tf_weapon_flamethrower")) return new CTFFlameThrower;
		if (!V_strcmp(cls, "tf_weapon_builder")) return new CTFWeaponBuilder;
		if (StartsWith(cls, "tf_weapon_grenade_")) return new CTFWeaponBaseGrenade;
		return new CTFWeaponProxy;
	}
	if (!V_strcmp(cls, "obj_sentrygun")) return new CObjectSentrygun;
	if (!V_strcmp(cls, "obj_dispenser")) return new CObjectDispenser;
	if (StartsWith(cls, "obj_teleporter")) return new CObjectTeleporter;
	if (StartsWith(cls, "obj_attachment_sapper")) return new CObjectSapper;
	if (StartsWith(cls, "obj_")) return new CBaseObject;
	if (!V_strcmp(cls, "item_teamflag")) return new CCaptureFlag;
	if (!V_strcmp(cls, "func_capturezone")) return new CCaptureZone;
	if (!V_strcmp(cls, "func_respawnroom")) return new CFuncRespawnRoom;
	if (!V_strcmp(cls, "team_control_point")) return new CTeamControlPoint;
	if (!V_strcmp(cls, "team_control_point_master")) return new CTeamControlPointMaster;
	if (!V_strcmp(cls, "trigger_capture_area")) return new CTriggerAreaCapture;
	if (!V_strcmp(cls, "team_train_watcher")) return new CTeamTrainWatcher;
	if (!V_strcmp(cls, "ff_team_manager")) return new CTFTeam;   // FFNB
	if (!V_strcmp(cls, "team_round_timer")) return new CTeamRoundTimer;
	if (!V_strcmp(cls, "func_brush")) return new CFuncBrush;
	if (!V_strcmp(cls, "worldspawn")) return new CWorld;
	if (StartsWith(cls, "func_door")) return new CBaseDoor;
	if (StartsWith(cls, "prop_door")) return new CBasePropDoor;
	if (StartsWith(cls, "prop_physics")) return new CPhysicsProp;
	return new CBaseEntity;
}

// ---------------------------------------------------------------------------------------------
// Class data (scripts/playerclasses/*.txt through the game filesystem, so VPK content works)
// ---------------------------------------------------------------------------------------------
static PF2ClassData s_classData[TF_CLASS_COUNT_ALL];
static bool s_classDataLoaded = false;
static const char *s_classNames[TF_CLASS_COUNT_ALL] = { "undefined", "scout", "sniper", "soldier", "demoman", "medic", "hwguy", "pyro", "spy", "engineer", "civilian" };
static const char *s_ammoNames[TF_AMMO_COUNT] = { "DUMMY AMMO", "TF_AMMO_PRIMARY", "TF_AMMO_SECONDARY", "TF_AMMO_METAL", "TF_AMMO_GRENADES1", "TF_AMMO_GRENADES2" };
struct WeaponName { const char *name; int id; };
static const WeaponName s_weaponNames[] = {
#include "pf2_weaponnames.inc"
};

static int WeaponIdFromName(const char *n)
{
	if (!n || !*n) return 0;
	for (const WeaponName &w : s_weaponNames)
		if (!V_stricmp(w.name, n))
			return w.id;
	return 0;
}

int PF2_ClassFromName(const char *name)
{
	if (!name) return 0;
	for (int i = 1; i < TF_CLASS_COUNT_ALL; ++i)
		if (!V_stricmp(name, s_classNames[i]))
			return i;
	if (!V_stricmp(name, "heavy")) return TF_CLASS_HEAVYWEAPONS;
	if (!V_stricmp(name, "demo")) return TF_CLASS_DEMOMAN;
	if (!V_stricmp(name, "engie")) return TF_CLASS_ENGINEER;
	return 0;
}

const char *PF2_ClassName(int c) { return (c >= 0 && c < TF_CLASS_COUNT_ALL) ? s_classNames[c] : "undefined"; }

// FFNB: FF ammo types in GetAmmoDef order (ff_gamerules.cpp); index 0 is "none".
static const char *s_ffAmmoNames[] = { "", "AMMO_NAILS", "AMMO_SHELLS", "AMMO_ROCKETS", "AMMO_CELLS", "AMMO_DETPACK", "AMMO_MANCANNON" };
static int FFAmmoIndex(const char *name)
{
	for (int i = 1; i < (int)ARRAYSIZE(s_ffAmmoNames); ++i)
		if (name && !V_stricmp(name, s_ffAmmoNames[i]))
			return i;
	return 0;
}

// ammo name a weapon uses, from scripts/<classname>.txt ("primary_ammo")
static void WeaponAmmoName(const char *weaponClass, char *out, int outLen)
{
	out[0] = 0;
	KeyValues *kv = new KeyValues("WeaponData");
	char path[128];
	V_snprintf(path, sizeof(path), "scripts/%s.txt", weaponClass);
	if (kv->LoadFromFile(filesystem, path, "GAME"))
		V_strncpy(out, kv->GetString("primary_ammo", ""), outLen);
	kv->deleteThis();
}

// "ff_grenade_concussion" -> TF_WEAPON_GRENADE_CONCUSSION (0 if TF has no equivalent, e.g. laser)
static int GrenadeIdFromClassname(const char *cls)
{
	if (!cls || V_strnicmp(cls, "ff_grenade_", 11))
		return 0;
	if (!V_stricmp(cls, "ff_grenade_laser") || !V_stricmp(cls, "ff_grenade_slowfield"))
		return TF_WEAPON_GRENADE_CALTROP;   // area denial, like PF2's caltrops
	char tfname[64];
	V_snprintf(tfname, sizeof(tfname), "TF_WEAPON_GRENADE_%s", cls + 11);
	V_strupr(tfname);
	return WeaponIdFromName(tfname);
}

void PF2_LoadClassData()
{
	// FFNB: FF class scripts are scripts/ff_playerclass_<name>.txt (PlayerClassData KeyValues)
	for (int c = 1; c < TF_CLASS_COUNT_ALL; ++c)
	{
		PF2ClassData &d = s_classData[c];
		memset(&d, 0, sizeof(d));
		V_strncpy(d.name, s_classNames[c], sizeof(d.name));
		d.maxHealth = 100; d.maxSpeed = 300.0f;
		KeyValues *kv = new KeyValues("PlayerClassData");
		char path[128];
		V_snprintf(path, sizeof(path), "scripts/ff_playerclass_%s.txt", s_classNames[c]);
		if (kv->LoadFromFile(filesystem, path, "GAME"))
		{
			d.maxHealth = kv->GetInt("health", 100);
			d.maxArmor = kv->GetInt("max_armour", 0);
			d.maxSpeed = kv->GetFloat("speed", 300.0f);
			int n = 0;
			const ff::WeaponInfo *best[2] = { NULL, NULL };
			if (KeyValues *arm = kv->FindKey("ArmamentsData"))
				for (KeyValues *w = arm->GetFirstValue(); w; w = w->GetNextValue())
				{
					const ff::WeaponInfo *info = ff::WeaponByClassname(w->GetString());
					if (!info)
						continue;
					if (n < 6)
						d.weapon[n++] = info->tfId;
					if (info->tfSlot == 0 || info->tfSlot == 1)
						if (!best[info->tfSlot] || info->rank > best[info->tfSlot]->rank)
							best[info->tfSlot] = info;
				}
			for (int g = 0; g < 2; ++g)
			{
				const char *gn = kv->GetString(g ? "secondary_classname" : "primary_classname");
				V_strncpy(d.grenadeName[g], gn, sizeof(d.grenadeName[g]));
				d.grenade[g] = GrenadeIdFromClassname(gn);
				d.maxAmmo[g ? TF_AMMO_GRENADES2 : TF_AMMO_GRENADES1] = kv->GetInt(g ? "secondary_max" : "primary_max", 0);
			}
			KeyValues *maxAmmo = kv->FindKey("MaxAmmoData");
			for (int slot = 0; slot < 2; ++slot)
			{
				int tfIdx = slot ? TF_AMMO_SECONDARY : TF_AMMO_PRIMARY;
				if (!best[slot])
					continue;
				char an[32];
				WeaponAmmoName(best[slot]->classname, an, sizeof(an));
				d.ffAmmoIndex[tfIdx] = FFAmmoIndex(an);
				d.maxAmmo[tfIdx] = maxAmmo ? maxAmmo->GetInt(an, 0) : 0;
			}
			d.ffAmmoIndex[TF_AMMO_METAL] = FFAmmoIndex("AMMO_CELLS");
			d.maxAmmo[TF_AMMO_METAL] = maxAmmo ? maxAmmo->GetInt("AMMO_CELLS", 0) : 0;
		}
		else
			Warning("[FFNB] could not read %s\n", path);
		kv->deleteThis();
	}
	s_classDataLoaded = true;
}

TFPlayerClassData_t *GetPlayerClassData(int iClass)
{
	static TFPlayerClassData_t data[TF_CLASS_COUNT_ALL];
	if (iClass < 0 || iClass >= TF_CLASS_COUNT_ALL)
		return NULL;
	TFPlayerClassData_t &d = data[iClass];
	V_strncpy(d.m_szClassName, s_classNames[iClass], sizeof(d.m_szClassName));
	if (const PF2ClassData *c = PF2_GetClassData(iClass))
	{
		d.m_flMaxSpeed = c->maxSpeed;
		d.m_nMaxHealth = c->maxHealth;
		d.m_nMaxArmor = c->maxArmor;
	}
	return &d;
}

int GetClassIndexFromString(const char *pClassName, int nLastClassIndex)
{
	int c = PF2_ClassFromName(pClassName);
	return (c && c <= nLastClassIndex) ? c : TF_CLASS_UNDEFINED;
}

static PF2NullLocalize s_nullLocalize;
PF2NullLocalize *g_pVGuiLocalize = &s_nullLocalize;

const PF2ClassData *PF2_GetClassData(int c)
{
	if (!s_classDataLoaded)
		PF2_LoadClassData();
	return (c > 0 && c < TF_CLASS_COUNT_ALL) ? &s_classData[c] : NULL;
}

// ---------------------------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------------------------
int CTFWeaponBase::GetWeaponID() const
{
	const ff::WeaponInfo *w = GetRaw() ? ff::WeaponByClassname(GetClassname()) : NULL;
	return w ? w->tfId : TF_WEAPON_NONE;
}

int CTFWeaponBase::GetSlot() const   // TFBot slot (0 primary, 1 secondary, 2 melee), -1 = utility
{
	const ff::WeaponInfo *w = GetRaw() ? ff::WeaponByClassname(GetClassname()) : NULL;
	return w ? w->tfSlot : -1;
}

bool CTFWeaponBase::IsMeleeWeapon() const
{
	switch (GetWeaponID())
	{
	case TF_WEAPON_BAT: case TF_WEAPON_BOTTLE: case TF_WEAPON_FIREAXE: case TF_WEAPON_CLUB: case TF_WEAPON_CROWBAR:
	case TF_WEAPON_KNIFE: case TF_WEAPON_FISTS: case TF_WEAPON_SHOVEL: case TF_WEAPON_WRENCH: case TF_WEAPON_BONESAW:
	case TF_WEAPON_BRANDINGIRON: case TF_WEAPON_MACHETE: case TF_WEAPON_MEDIKIT: case TF_WEAPON_PIPE:
	case TF_WEAPON_SYRINGE: case TF_WEAPON_UMBRELLA:	// PF2: medic's melee syringe, civilian's umbrella
		return true;
	}
	return GetSlot() == 2;
}

bool CTFWeaponBase::IsGrenade() const
{
	int id = GetWeaponID();
	return id >= TF_WEAPON_GRENADE_NORMAL && id <= TF_WEAPON_GRENADE_HEAL;
}

float CTFWeaponBase::GetProjectileSpeed()
{
	const ff::WeaponInfo *w = GetRaw() ? ff::WeaponByClassname(GetClassname()) : NULL;
	return w ? w->projectileSpeed : 0.0f;
}

const CTFWeaponInfo &CTFWeaponBase::GetTFWpnData() const
{
	m_info.m_WeaponData[0].m_flProjectileSpeed = const_cast<CTFWeaponBase *>(this)->GetProjectileSpeed();
	return m_info;
}

int CTFWeaponBase::GetMaxClip1() const { int *p = Field<int>("m_iMaxClip1"); return p ? *p : -1; }
CTFPlayer *CTFWeaponBase::GetTFPlayerOwner() const { return ToTFPlayer(GetOwner()); }

bool CTFWeaponBase::IsZoomed() const
{
	CTFPlayer *o = GetTFPlayerOwner();
	return o && o->m_Shared.InCond(TF_COND_ZOOMED);
}

float CTFWeaponBase::GetTimeSinceLastFired() const
{
	float *p = Field<float>("m_flLastFireTime");
	return p && *p > 0 ? gpGlobals->curtime - *p : 99999.9f;
}

// ---------------------------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------------------------
CTFPlayer::CTFPlayer()
{
	m_Shared.m_pOuter = this;
	m_PlayerClass.m_pOuter = this;
}

int CTFPlayerClass::GetClassIndex() const
{
	// FFNB: FF packs the class into the low nibble of m_iClassStatus (CFFPlayer::GetClassSlot).
	// FF's class numbers (scout 1 .. engineer 9, civilian 10) equal TF2's.
	int *p = m_pOuter ? m_pOuter->Field<int>("m_iClassStatus") : NULL;
	return p ? (*p & 0x0F) : TF_CLASS_UNDEFINED;
}

bool CTFPlayerShared::InCond(int nCond) const
{
	// FFNB: FF has no condition bitfield; map the TF conditions TFBot asks about onto FF state.
	if (!m_pOuter)
		return false;
	auto i32 = [this](const char *f) { int *p = m_pOuter->SendField<int>(f); return p ? *p : 0; };
	auto b8 = [this](const char *f) { bool *p = m_pOuter->SendField<bool>(f); return p && *p; };
	switch (nCond)
	{
	case TF_COND_DISGUISED:     return m_pOuter->GetPlayerClass()->GetClassIndex() == TF_CLASS_SPY && i32("m_iSpyDisguise") != 0;
	case TF_COND_DISGUISING:    return i32("m_iSpyDisguising") != 0;
	case TF_COND_STEALTHED:     return i32("m_iCloaked") != 0;
	case TF_COND_BURNING:       return (i32("m_fFlags") & FL_ONFIRE) != 0;
	case TF_COND_INFECTED:      return b8("m_bInfected");
	case TF_COND_TRANQUILIZED:  return b8("m_bTranqed");
	case TF_COND_ZOOMED:
	{
		int fov = i32("m_iFOV"), def = i32("m_iDefaultFOV");
		return fov > 0 && def > 0 && fov < def;
	}
	}
	return false;
}

bool CTFPlayerShared::IsCarryingObject() const { bool *p = m_pOuter ? m_pOuter->Field<bool>("m_bCarryingObject") : NULL; return p && *p; }
// FFNB: m_iSpyDisguise = team | class << 4 (CFFPlayer::GetDisguisedTeam/Class)
int CTFPlayerShared::GetDisguiseTeam() const { return InCond(TF_COND_DISGUISED) ? (*m_pOuter->Field<int>("m_iSpyDisguise") & 0x0F) : TEAM_UNASSIGNED; }
int CTFPlayerShared::GetDisguiseClass() const { return InCond(TF_COND_DISGUISED) ? ((*m_pOuter->Field<int>("m_iSpyDisguise") & 0xF0) >> 4) : 0; }
int CTFPlayerShared::GetNumHealers() const { int *p = m_pOuter ? m_pOuter->Field<int>("m_nNumHealers") : NULL; return p ? *p : 0; }
int CTFPlayerShared::GetMaxBuffedHealth() const { return m_pOuter ? (int)(m_pOuter->GetMaxHealth() * 1.5f) : 150; }
float CTFPlayerShared::GetSpyCloakMeter() const { float *p = m_pOuter ? m_pOuter->Field<float>("m_flCloakMeter") : NULL; return p ? *p : 0.0f; }
CTFWeaponBase *CTFPlayerShared::GetActiveTFWeapon() const { return m_pOuter ? m_pOuter->GetActiveTFWeapon() : NULL; }

float CTFPlayerShared::GetPercentInvisible() const
{
	if (!InCond(TF_COND_STEALTHED))
		return 0.0f;
	float *done = m_pOuter->Field<float>("m_flInvisChangeCompleteTime");
	if (!done || gpGlobals->curtime >= *done)
		return 1.0f;
	return clamp(1.0f - (*done - gpGlobals->curtime), 0.0f, 1.0f);
}

void CTFPlayerShared::Disguise(int nTeam, int nClass, CTFPlayer *, bool)
{
	if (m_pOuter)
	{
		// FFNB: FF's "disguise <team> <class>"; the team is a digit 1-4 (blue..green) or "enemy"
		const char *team = (nTeam >= TF_TEAM_BLUE && nTeam < TF_TEAM_COUNT) ? UTIL_VarArgs("%d", nTeam - 1) : "enemy";
		char teamArg[8];
		V_strncpy(teamArg, team, sizeof(teamArg));
		m_pOuter->ClientCommand(UTIL_VarArgs("disguise %s %s", teamArg, PF2_ClassName(nClass)));
	}
}

// FFNB: FF stages a class change in m_iNextClass, which is neither networked nor in the datamap.
// We remember the class we last asked for per slot (bot creation and our "class" commands).
static int s_ffDesiredClass[MAX_PLAYERS + 1];
void FF_NoteDesiredClass(int entindex, const char *className)
{
	if (entindex >= 1 && entindex <= MAX_PLAYERS)
		s_ffDesiredClass[entindex] = PF2_ClassFromName(className);
}
int CTFPlayer::GetDesiredPlayerClassIndex() const
{
	int idx = entindex();
	if (idx >= 1 && idx <= MAX_PLAYERS && s_ffDesiredClass[idx] > TF_CLASS_UNDEFINED)
		return s_ffDesiredClass[idx];
	return m_PlayerClass.GetClassIndex();
}

CTFWeaponBase *CTFPlayer::Weapon_OwnsThisID(int id) const
{
	for (int i = 0; i < MAX_WEAPONS; ++i)
	{
		CTFWeaponBase *w = ToTFWeaponBase(GetWeapon(i));
		if (w && w->GetWeaponID() == id)
			return w;
	}
	return NULL;
}

CTFWeaponBase *CTFPlayer::Weapon_GetWeaponByType(int iType)
{
	for (int i = 0; i < MAX_WEAPONS; ++i)
	{
		CTFWeaponBase *w = ToTFWeaponBase(GetWeapon(i));
		if (!w)
			continue;
		int slot = w->GetSlot();
		if ((iType == TF_WPN_TYPE_PRIMARY && slot == 0) || (iType == TF_WPN_TYPE_SECONDARY && slot == 1) ||
			(iType == TF_WPN_TYPE_MELEE && slot == 2) || (iType == TF_WPN_TYPE_PDA && slot == 3) ||
			(iType == TF_WPN_TYPE_BUILDING && w->ClassMatches("tf_weapon_builder")))
			return w;
	}
	return NULL;
}

bool CTFPlayer::HasTheFlag() const
{
	return GetItem() != NULL;
}

int CTFPlayer::GetMaxAmmo(int iAmmoIndex, int iClassNumber) const
{
	const PF2ClassData *d = PF2_GetClassData(iClassNumber > 0 ? iClassNumber : m_PlayerClass.GetClassIndex());
	int m = (d && iAmmoIndex >= 0 && iAmmoIndex < TF_AMMO_COUNT) ? d->maxAmmo[iAmmoIndex] : 0;
	return m > 0 ? m : 1;   // FFNB: never 0 (TFBot divides by it)
}

int CTFPlayer::GetAmmoCount(int tfAmmoIndex) const
{
	if (tfAmmoIndex == TF_AMMO_GRENADES1 || tfAmmoIndex == TF_AMMO_GRENADES2)
	{
		int *p = SendField<int>(tfAmmoIndex == TF_AMMO_GRENADES1 ? "m_iPrimary" : "m_iSecondary");
		return p ? *p : 0;
	}
	const PF2ClassData *d = PF2_GetClassData(m_PlayerClass.GetClassIndex());
	if (!d || tfAmmoIndex < 0 || tfAmmoIndex >= TF_AMMO_COUNT)
		return 0;
	if (!d->ffAmmoIndex[tfAmmoIndex])
		return GetMaxAmmo(tfAmmoIndex);   // no such ammo for this class: report "full"
	return CBaseCombatCharacter::GetAmmoCount(d->ffAmmoIndex[tfAmmoIndex]);
}

// FFNB: FF has no networked throw cooldown; a grenade is primed while m_iGrenadeState != GS_NONE
float CTFPlayer::GetNextThrowTime() const { return 0.0f; }
bool CTFPlayer::IsGrenadePrimed() const { int *p = SendField<int>("m_iGrenadeState"); return p && *p != 0; }

Vector CTFPlayer::GetClassEyeHeight()
{
	Vector *v = Field<Vector>("m_vecViewOffset");
	return Vector(0, 0, v ? v->z : 68.0f);
}

void CTFPlayer::StartBuildingObjectOfType(int iType, int iMode)
{
	// FFNB: FF builds in front of the engineer at once; the same command detonates an existing one,
	// so never issue it while we already own that building. FF has no teleporters.
	if (GetObjectOfType(iType, iMode))
		return;
	if (iType == OBJ_SENTRYGUN)
		ClientCommand("sentrygun");
	else if (iType == OBJ_DISPENSER)
		ClientCommand("dispenser");
}

// ---- projectile impact estimation: Valve CTFPlayer code (Source SDK 2013, tf_player.cpp) ----
Vector CTFPlayer::EstimateProjectileImpactPosition(CTFWeaponBaseGun *weapon)
{
	if (!weapon)
		return GetAbsOrigin();
	const QAngle &angles = EyeAngles();
	float initVel = weapon->IsWeapon(TF_WEAPON_PIPEBOMBLAUNCHER) ? 900.0f : weapon->GetProjectileSpeed();
	return EstimateProjectileImpactPosition(angles.x, angles.y, initVel);
}

Vector CTFPlayer::EstimateStickybombProjectileImpactPosition(float pitch, float yaw, float charge)
{
	float initVel = charge * (TF_PIPEBOMB_MAX_CHARGE_VEL - TF_PIPEBOMB_MIN_CHARGE_VEL) + TF_PIPEBOMB_MIN_CHARGE_VEL;
	return EstimateProjectileImpactPosition(pitch, yaw, initVel);
}

Vector CTFPlayer::EstimateProjectileImpactPosition(float pitch, float yaw, float initVel)
{
	Vector vecForward, vecRight, vecUp;
	QAngle angles(pitch, yaw, 0.0f);
	AngleVectors(angles, &vecForward, &vecRight, &vecUp);
	float fRight = 8.f;
	Vector vecSrc = Weapon_ShootPosition();
	vecSrc += vecForward * 16.0f + vecRight * fRight + vecUp * -6.0f;
	const float initVelScale = 0.9f;
	Vector vecVelocity = initVelScale * ((vecForward * initVel) + (vecUp * 200.0f));
	const float timeStep = 0.01f;
	const float maxTime = 5.0f;
	Vector pos = vecSrc;
	Vector lastPos = pos;
	static ConVarRef sv_gravity("sv_gravity");
	const float g = sv_gravity.IsValid() ? sv_gravity.GetFloat() : 800.0f;
	Vector alongDir = vecForward;
	alongDir.z = 0.0f;
	alongDir.NormalizeInPlace();
	float alongVel = FastSqrt(vecVelocity.x * vecVelocity.x + vecVelocity.y * vecVelocity.y);
	trace_t trace;
	trace.endpos = vec3_origin;
	CTraceFilterNoCombatCharacters traceFilter(NULL, COLLISION_GROUP_NONE);
	for (float t = 0.0f; t < maxTime; t += timeStep)
	{
		float along = alongVel * t;
		float height = vecVelocity.z * t - 0.5f * g * t * t;
		pos.x = vecSrc.x + alongDir.x * along;
		pos.y = vecSrc.y + alongDir.y * along;
		pos.z = vecSrc.z + height;
		UTIL_TraceHull(lastPos, pos, -Vector(8, 8, 8), Vector(8, 8, 8), MASK_SOLID_BRUSHONLY, &traceFilter, &trace);
		if (trace.DidHit())
			break;
		lastPos = pos;
	}
	return trace.endpos;
}

CUtlVector<CHandle<CTFGrenadePipebombProjectile> > &CTFPipebombLauncher::GetPipeBombVector()
{
	static CUtlVector<CHandle<CTFGrenadePipebombProjectile> > v;
	v.RemoveAll();
	CBaseEntity *owner = GetOwner();
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "tf_projectile_pipe_remote"); e; e = gEntList.FindEntityByClassname(e, "tf_projectile_pipe_remote"))
		if (e->HandleField("m_hThrower") == owner || e->GetOwnerEntity() == owner)
			v.AddToTail(static_cast<CTFGrenadePipebombProjectile *>(e));
	return v;
}

// m_hObjectBeingBuilt is neither networked nor in the datamap in PF2; the blueprint is the object
// this engineer owns that is still being placed.
CBaseEntity *CTFWeaponBuilder::GetPlacementObject() const
{
	CBaseEntity *owner = GetOwner();
	if (!owner)
		return NULL;
	for (int i = 0; i < IBaseObjectAutoList::AutoList().Count(); ++i)
	{
		CBaseObject *o = static_cast<CBaseObject *>(IBaseObjectAutoList::AutoList()[i]);
		if (o->GetBuilder() == owner && o->IsPlacing())
			return o;
	}
	return NULL;
}

// PF2's CTFWeaponBuilder::IsValidPlacement is the blueprint's UpdatePlacement(), whose result is the
// virtual CBaseObject::IsPlacementPosValid() (computes the build spot from the owner's aim).
// PF2's own PrimaryAttack re-validates and moves the blueprint when the bot presses fire.
bool CTFWeaponBuilder::IsValidPlacement() const
{
	CBaseEntity *obj = GetPlacementObject();
	void *raw = obj ? obj->GetRaw() : NULL;
	return raw && VCall<bool>(raw, 336 /* CBaseObject::IsPlacementPosValid */);
}

int CBaseObject::GetPF2Type() const
{
	const char *c = GetClassname();
	if (!V_stricmp(c, "FF_SentryGun")) return OBJ_SENTRYGUN;
	if (!V_stricmp(c, "FF_Dispenser")) return OBJ_DISPENSER;
	return -1;
}

void CBaseObject::DetonateObject()
{
	// FFNB: FF's "sentrygun" / "dispenser" commands detonate an existing building
	CTFPlayer *b = GetBuilder();
	if (b && (GetType() == OBJ_SENTRYGUN || GetType() == OBJ_DISPENSER))
		b->ClientCommand(GetType() == OBJ_SENTRYGUN ? "sentrygun" : "dispenser");
}

#define PF2_AUTOLIST(Iface, Proxy, cls) 	const CUtlVector<Iface *> &Iface::AutoList() 	{ 		static CUtlVector<Iface *> list; static int tick = -1; 		if (tick != gpGlobals->tickcount) { tick = gpGlobals->tickcount; list.RemoveAll(); 			for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, cls); e; e = gEntList.FindEntityByClassname(e, cls)) 				if (Proxy *p = dynamic_cast<Proxy *>(e)) list.AddToTail(p); } 		return list; 	}
bool PointInRespawnRoom(const CBaseEntity *pTarget, const Vector &vecOrigin, bool bTouching_SameTeamOnly)
{
	const IFuncRespawnRoomAutoList *dummy = NULL; (void)dummy;
	for (int i = 0; i < IFuncRespawnRoomAutoList::AutoList().Count(); ++i)
	{
		CFuncRespawnRoom *room = static_cast<CFuncRespawnRoom *>(IFuncRespawnRoomAutoList::AutoList()[i]);
		if (!room->GetActive() || room->GetDisabled())
			continue;
		if (bTouching_SameTeamOnly && pTarget && room->GetTeamNumber() != pTarget->GetTeamNumber())
			continue;
		if (room->PointIsWithin(vecOrigin))
			return true;
	}
	return false;
}

// FFNB: flags and capture zones are the census's Lua objectives, not TF entities
#define FF_CENSUS_AUTOLIST(Iface, Proxy, kindTest) 	const CUtlVector<Iface *> &Iface::AutoList() 	{ 		static CUtlVector<Iface *> list; static int tick = -1; 		if (tick != gpGlobals->tickcount) { tick = gpGlobals->tickcount; list.RemoveAll(); 			for (int i = 0; i < ff::Census_Count(); ++i) { const ff::CensusObj *o = ff::Census_At(i); 				if (!(kindTest)) continue; 				if (Proxy *p = dynamic_cast<Proxy *>(PF2_EntityByIndex(o->ent))) list.AddToTail(p); } } 		return list; 	}
FF_CENSUS_AUTOLIST(ICaptureZoneAutoList, CCaptureZone, o->kind == ff::OBJ_CAP)
FF_CENSUS_AUTOLIST(ICaptureFlagAutoList, CCaptureFlag, o->kind == ff::OBJ_FLAG || o->kind == ff::OBJ_BALL)   // a ball is a neutral flag

bool CCaptureFlag::IsDisabled() const
{
	const ff::CensusObj *o = ff::Census_Get(entindex());
	return GetPosState() == 3 || GetGoalState() == 2 || (o && o->disabled);
}

bool CCaptureZone::IsDisabled() const
{
	const ff::CensusObj *o = ff::Census_Get(entindex());
	return o && o->disabled;
}

bool CCaptureZone::Accepts(const CBaseEntity *flag) const
{
	const ff::CensusObj *f = flag ? ff::Census_Get(flag->entindex()) : NULL;
	return f && ff::Census_CapAccepts(ff::Census_Get(entindex()), f->name);
}

static bool ZoneUsableBy(const CCaptureZone *zone, int team)
{
	int zt = zone->GetTeamNumber();
	return !zone->IsDisabled() && (zt == team || zt <= 0);
}

bool FF_TeamCanCaptureFlag(int team, const CCaptureFlag *flag)
{
	if (!flag || flag->IsDisabled())
		return false;
	for (int i = 0; i < ICaptureZoneAutoList::AutoList().Count(); ++i)
	{
		CCaptureZone *zone = static_cast<CCaptureZone *>(ICaptureZoneAutoList::AutoList()[i]);
		if (ZoneUsableBy(zone, team) && zone->Accepts(flag))
			return true;
	}
	return false;
}

CCaptureZone *FF_CaptureZoneFor(int team, const CCaptureFlag *flag, const Vector &from)
{
	CCaptureZone *best = NULL;
	float bestDist = FLT_MAX;
	for (int i = 0; i < ICaptureZoneAutoList::AutoList().Count(); ++i)
	{
		CCaptureZone *zone = static_cast<CCaptureZone *>(ICaptureZoneAutoList::AutoList()[i]);
		if (!ZoneUsableBy(zone, team) || (flag && !zone->Accepts(flag)))
			continue;
		float d = (zone->WorldSpaceCenter() - from).LengthSqr();
		if (d < bestDist)
		{
			bestDist = d;
			best = zone;
		}
	}
	return best;
}

CBaseEntity *CTFPlayer::GetItem() const
{
	for (int i = 0; i < ICaptureFlagAutoList::AutoList().Count(); ++i)
	{
		CCaptureFlag *flag = static_cast<CCaptureFlag *>(ICaptureFlagAutoList::AutoList()[i]);
		if (flag->IsStolen() && flag->GetCarrier() == this)
			return flag;
	}
	return NULL;
}
PF2_AUTOLIST(IFuncRespawnRoomAutoList, CFuncRespawnRoom, "func_respawnroom")
PF2_AUTOLIST(ITFTeamSpawnAutoList, CTFTeamSpawn, "info_ff_teamspawn")   // FFNB

int CTFTeamSpawn::GetTeamNumber() const
{
	int t = CBaseEntity::GetTeamNumber();
	if (t >= TF_TEAM_BLUE && t < TF_TEAM_COUNT)
		return t;
	char name[128];
	V_strncpy(name, STRING(GetEntityName()), sizeof(name));
	V_strlower(name);
	static const struct { const char *s; int team; } k[] = {
		{ "blue", TF_TEAM_BLUE }, { "red", TF_TEAM_RED }, { "yellow", TF_TEAM_YELLOW }, { "green", TF_TEAM_GREEN } };
	int best = 0, bestPos = 1 << 30;
	for (auto &e : k)
	{
		const char *p = V_strstr(name, e.s);
		if (p && p - name < bestPos)
		{
			bestPos = (int)(p - name);
			best = e.team;
		}
	}
	return best;
}
bool g_fGameOver = false;
class CPopulationManager;
CPopulationManager *g_pPopulationManager = NULL;

CBaseEntity *CTFPlayer::MedicGetHealTarget()
{
	CWeaponMedigun *m = dynamic_cast<CWeaponMedigun *>(GetActiveTFWeapon());
	return m ? m->GetHealTarget() : NULL;
}

float CTFPlayer::MedicGetChargeLevel(CTFWeaponBase **pRetMedigun)
{
	CWeaponMedigun *m = dynamic_cast<CWeaponMedigun *>(Weapon_OwnsThisID(TF_WEAPON_MEDIGUN));
	if (pRetMedigun) *pRetMedigun = m;
	return m ? m->GetChargeLevel() : 0.0f;
}

bool CTFPlayer::IsThreatAimingTowardMe(CBaseEntity *threat, float cosTolerance) const
{
	CTFPlayer *p = ToTFPlayer(threat);
	if (!p)
		return false;
	Vector to = const_cast<CTFPlayer *>(this)->WorldSpaceCenter() - p->EyePosition();
	to.NormalizeInPlace();
	Vector fwd;
	p->EyeVectors(&fwd);
	return DotProduct(to, fwd) > cosTolerance;
}

CTriggerAreaCapture *CTFPlayer::GetControlPointStandingOn()
{
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "trigger_capture_area"); e; e = gEntList.FindEntityByClassname(e, "trigger_capture_area"))
		if (e->CollisionProp()->IsPointInBounds(WorldSpaceCenter()))
			return static_cast<CTriggerAreaCapture *>(e);
	return NULL;
}

bool CTFPlayer::IsCapturingPoint()
{
	CTriggerAreaCapture *t = GetControlPointStandingOn();
	CTeamControlPoint *p = t ? t->GetControlPoint() : NULL;
	return p && p->GetOwner() != GetTeamNumber();
}

void CCaptureFlag::AddFollower(CTFBot *bot)
{
	CTFPlayer *p = ToTFPlayer((CBaseEntity *)bot);
	if (p && m_followers.Find(p) == m_followers.InvalidIndex())
		m_followers.AddToTail(p);
}

void CCaptureFlag::RemoveFollower(CTFBot *bot)
{
	m_followers.FindAndRemove(ToTFPlayer((CBaseEntity *)bot));
}

static CTFWeaponInfo s_weaponInfo[TF_WEAPON_COUNT + 1];
WEAPON_FILE_INFO_HANDLE LookupWeaponInfoSlot(const char *name) { int id = WeaponIdFromName(name); return id > 0 ? (WEAPON_FILE_INFO_HANDLE)id : GetInvalidWeaponInfoHandle(); }
WEAPON_FILE_INFO_HANDLE GetInvalidWeaponInfoHandle(void) { return (WEAPON_FILE_INFO_HANDLE)0xFFFF; }
FileWeaponInfo_t *GetFileWeaponInfoFromHandle(WEAPON_FILE_INFO_HANDLE h)
{
	if (h == 0 || h > TF_WEAPON_COUNT)
		return NULL;
	CTFWeaponInfo &w = s_weaponInfo[h];
	// weapons that never use ammo: medigun, PDAs, builder, invisibility watch
	bool noAmmo = h == TF_WEAPON_MEDIGUN || h == TF_WEAPON_PDA || h == TF_WEAPON_PDA_ENGINEER_BUILD || h == TF_WEAPON_PDA_ENGINEER_DESTROY ||
		h == TF_WEAPON_PDA_SPY || h == TF_WEAPON_BUILDER || h == TF_WEAPON_INVIS || h == TF_WEAPON_PDA_DEMO;
	w.m_WeaponData[0].m_iProjectile = noAmmo ? TF_PROJECTILE_NONE : TF_PROJECTILE_BULLET;
	return &w;
}
const char *WeaponIdToAlias(int iWeapon)
{
	for (const WeaponName &w : s_weaponNames)
		if (w.id == iWeapon)
			return w.name;
	return NULL;
}

CBaseObject *CTFPlayer::GetObjectOfType(int iType, int iMode) const
{
	// FFNB: FF buildings (including ones still being built: TFBot then waits for them to finish)
	for (int i = 0; i < IBaseObjectAutoList::AutoList().Count(); ++i)
	{
		CBaseObject *o = static_cast<CBaseObject *>(IBaseObjectAutoList::AutoList()[i]);
		if (o->GetBuilder() == this && o->GetType() == iType)
			return o;
	}
	return NULL;
}

int CTFPlayer::GetObjectCount() const
{
	int n = 0;
	for (int i = 0; i < IBaseObjectAutoList::AutoList().Count(); ++i)
		if (static_cast<CBaseObject *>(IBaseObjectAutoList::AutoList()[i])->GetBuilder() == this)
			++n;
	return n;
}

CBaseObject *CTFPlayer::GetObject(int idx) const
{
	int n = 0;
	for (int i = 0; i < IBaseObjectAutoList::AutoList().Count(); ++i)
	{
		CBaseObject *o = static_cast<CBaseObject *>(IBaseObjectAutoList::AutoList()[i]);
		if (o->GetBuilder() == this && n++ == idx)
			return o;
	}
	return NULL;
}

CTFTeam *CTFPlayer::GetTFTeam() const { return GetGlobalTFTeam(GetTeamNumber()); }
CTFTeam *CTFPlayer::GetOpposingTFTeam() const { return GetGlobalTFTeam(GetTeamNumber() == TF_TEAM_RED ? TF_TEAM_BLUE : TF_TEAM_RED); }

// FFNB: FF's "class <name>" and "team <name>" (names: see s_classNames; teams blue/red/yellow/green/spec)
void CTFPlayer::HandleCommand_JoinClass(const char *pClassName, bool)
{
	if (!V_stricmp(pClassName, "heavyweapons") || !V_stricmp(pClassName, "heavy"))
		pClassName = "hwguy";
	FF_NoteDesiredClass(entindex(), pClassName);
	ClientCommand(UTIL_VarArgs("class %s", pClassName));
}
void CTFPlayer::HandleCommand_JoinTeam(const char *pTeamName)
{
	if (!V_stricmp(pTeamName, "spectate") || !V_stricmp(pTeamName, "spectator"))
		pTeamName = "spec";
	else if (!V_stricmp(pTeamName, "auto"))
		pTeamName = "random";
	ClientCommand(UTIL_VarArgs("team %s", pTeamName));
}
void CTFPlayer::ForceChangeTeam(int iTeamNum, bool) { ChangeTeam(iTeamNum); }

void CTFPlayer::ForceRespawn()
{
	void *raw = GetRaw();
	if (raw)
		VCall<void>(raw, vt::ForceRespawn);
}

// ---------------------------------------------------------------------------------------------
// Buildings / objectives
// ---------------------------------------------------------------------------------------------
const CUtlVector<IBaseObjectAutoList *> &IBaseObjectAutoList::AutoList()
{
	static CUtlVector<IBaseObjectAutoList *> list;
	static int builtTick = -1;
	if (builtTick != gpGlobals->tickcount)
	{
		builtTick = gpGlobals->tickcount;
		list.RemoveAll();
		static const char *ffObjects[] = { "FF_SentryGun", "FF_Dispenser" };   // FFNB
		for (const char *cls : ffObjects)
			for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, cls); e; e = gEntList.FindEntityByClassname(e, cls))
				if (e->IsBaseObject())
					list.AddToTail(static_cast<CBaseObject *>(e));
	}
	return list;
}

CObjectTeleporter *CObjectTeleporter::GetMatchingTeleporter()
{
	CTFPlayer *builder = GetBuilder();
	if (!builder)
		return NULL;
	return static_cast<CObjectTeleporter *>(builder->GetObjectOfType(OBJ_TELEPORTER, IsEntrance() ? MODE_TELEPORTER_EXIT : MODE_TELEPORTER_ENTRANCE));
}

CBaseEntity *CCaptureFlag::GetCarrier() const
{
	if (!IsStolen())
		return NULL;
	CBaseEntity *self = const_cast<CCaptureFlag *>(this);
	CBaseEntity *p = self->GetMoveParent();
	if (p && p->IsPlayer())
		return p;
	CBaseEntity *o = GetOwnerEntity();
	return (o && o->IsPlayer()) ? o : NULL;
}

bool CFuncRespawnRoom::PointIsWithin(const Vector &vecPoint)
{
	// brush entity: test the point against the brush model with an engine trace
	edict_t *ed = edict();
	if (!ed || !GetRaw())
		return false;
	Ray_t ray;
	ray.Init(vecPoint, vecPoint);
	trace_t tr;
	enginetrace->ClipRayToEntity(ray, MASK_ALL, (IHandleEntity *)GetRaw(), &tr);
	return tr.startsolid;
}

CTeamControlPoint *CTriggerAreaCapture::GetControlPoint() const
{
	CBaseEntity *p = HandleField("m_hPoint");
	if (p)
		return static_cast<CTeamControlPoint *>(p);
	string_t *name = Field<string_t>("m_iszCapPointName");
	if (name && STRING(*name))
		return static_cast<CTeamControlPoint *>(gEntList.FindEntityByName(NULL, STRING(*name)));
	return NULL;
}

bool CTeamControlPoint::IsLocked()
{
	bool *p = Field<bool>("m_bLocked");
	return p && *p;
}

float CTeamControlPoint::GetTeamCapPercentage(int iTeam)
{
	// objective resource: lazy cap percentage is the capping progress (0..1) of the capping team
	CTFObjectiveResource *res = TFObjectiveResource();
	int idx = GetPointIndex();
	if (res->GetCappingTeam(idx) != iTeam)
		return GetOwner() == iTeam ? 1.0f : 0.0f;
	return res->GetCPCapPercentage(idx);
}

CUtlVector<CHandle<CTeamControlPointMaster> > g_hControlPointMasters;

// The game fills g_hControlPointMasters as masters spawn; here it is rebuilt from the entity list.
void PF2_RefreshControlPointMasters()
{
	g_hControlPointMasters.RemoveAll();
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "team_control_point_master"); e; e = gEntList.FindEntityByClassname(e, "team_control_point_master"))
	{
		bool *disabled = e->Field<bool>("m_bDisabled");
		if (!disabled || !*disabled)
			g_hControlPointMasters.AddToTail(static_cast<CTeamControlPointMaster *>(e));
	}
}

int CTeamControlPointMaster::GetNumPoints() { return TFObjectiveResource()->GetNumControlPoints(); }

CTeamControlPoint *CTeamControlPointMaster::GetControlPoint(int point)
{
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "team_control_point"); e; e = gEntList.FindEntityByClassname(e, "team_control_point"))
	{
		CTeamControlPoint *cp = static_cast<CTeamControlPoint *>(e);
		if (cp->GetPointIndex() == point)
			return cp;
	}
	return NULL;
}

CBaseEntity *CTeamTrainWatcher::GetTrainEntity() const
{
	CBaseEntity *t = HandleField("m_hTrain");
	if (t)
		return t;
	string_t *name = Field<string_t>("m_iszTrain");
	return (name && STRING(*name)) ? gEntList.FindEntityByName(NULL, STRING(*name)) : NULL;
}

Vector CTeamTrainWatcher::GetNextCheckpointPosition() const
{
	CBaseEntity *t = GetTrainEntity();
	return t ? t->GetAbsOrigin() : vec3_origin;
}

void CTeamTrainWatcher::ProjectPointOntoPath(const Vector &pos, Vector *pathPos, float *distanceAlongPath) const
{
	// PF2NB: the path_track chain is not reachable from an addon; approximate with the train itself
	CBaseEntity *t = GetTrainEntity();
	if (pathPos) *pathPos = t ? t->GetAbsOrigin() : pos;
	if (distanceAlongPath) *distanceAlongPath = 0.0f;
}

bool CTeamTrainWatcher::IsAheadOfTrain(const Vector &) const { return false; }

// ---------------------------------------------------------------------------------------------
// Teams
// ---------------------------------------------------------------------------------------------
const char *CTeam::GetName() const
{
	const char *p = Field<const char>("m_szTeamname");
	return p ? p : "";
}

int CTeam::GetNumPlayers() const
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		if (p && p->GetTeamNumber() == GetTeamNumber())
			++n;
	}
	return n;
}

CBasePlayer *CTeam::GetPlayer(int iIndex) const
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		if (p && p->GetTeamNumber() == GetTeamNumber() && n++ == iIndex)
			return p;
	}
	return NULL;
}

CTeam *GetGlobalTeam(int iIndex)
{
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "ff_team_manager"); e; e = gEntList.FindEntityByClassname(e, "ff_team_manager"))
		if (static_cast<CTeam *>(e)->GetTeamNumber() == iIndex)
			return static_cast<CTeam *>(e);
	return NULL;
}

CTFTeam *GetGlobalTFTeam(int iIndex) { return static_cast<CTFTeam *>(GetGlobalTeam(iIndex)); }

bool FF_AreTeamsFriendly(int a, int b)
{
	if (a < FIRST_GAME_TEAM || b < FIRST_GAME_TEAM)
		return a == b;
	CTFTeam *ta = GetGlobalTFTeam(a), *tb = GetGlobalTFTeam(b);
	if (a == b)
		return !(ta && ta->IsFFA());
	return (ta && (ta->GetAllies() & (1 << b))) || (tb && (tb->GetAllies() & (1 << a)));
}

int GetEnemyTeam(int iTeam)
{
	if (iTeam < FIRST_GAME_TEAM || iTeam >= TF_TEAM_COUNT)
		return iTeam;
	static const int partner[TF_TEAM_COUNT] = { 0, 0, TF_TEAM_RED, TF_TEAM_BLUE, TF_TEAM_GREEN, TF_TEAM_YELLOW };
	int p = partner[iTeam];
	CTeam *pt = GetGlobalTeam(p);
	if (!FF_AreTeamsFriendly(iTeam, p) && pt && pt->GetNumPlayers() > 0)
		return p;
	int best = 0, bestPlayers = -1;
	for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
	{
		CTeam *tt = GetGlobalTeam(t);
		if (t == iTeam || FF_AreTeamsFriendly(iTeam, t) || !tt)
			continue;
		if (tt->GetNumPlayers() > bestPlayers)
		{
			bestPlayers = tt->GetNumPlayers();
			best = t;
		}
	}
	return best ? best : p;
}
int GetNumberOfTeams() { return TF_TEAM_COUNT; }
CTeam *CBaseEntity::GetTeam() const { return GetGlobalTeam(GetTeamNumber()); }

// ---------------------------------------------------------------------------------------------
// Game rules
// ---------------------------------------------------------------------------------------------
static CTFGameRules s_tfGameRules;
CGameRules *g_pGameRules = &s_tfGameRules;
CTFGameRules *TFGameRules() { return &s_tfGameRules; }

// FFNB: Fortress Forever's player hull (gamerules.cpp g_DefaultViewVectors: 32x32x72, crouched 36,
// eyes 28 / 12 above the centre), expressed relative to the feet because the shim reports a
// player's origin at its feet (CBaseEntity::GetAbsOrigin)
static CViewVectors s_viewVectors(
	Vector(0, 0, 64), Vector(-16, -16, 0), Vector(16, 16, 72), Vector(-16, -16, 0), Vector(16, 16, 36),
	Vector(0, 0, 30), Vector(-10, -10, -10), Vector(10, 10, 10), Vector(0, 0, 34));
const CViewVectors *CGameRules::GetViewVectors() const { return &s_viewVectors; }

// the gamerules object lives behind the proxy entity's datatable proxy function (as SourceMod does it)
static void *GameRulesObject(int *outBaseOffsetOfTable = NULL)
{
	CBaseEntity *proxy = gEntList.FindEntityByClassname(NULL, "ff_gamerules");
	void *raw = proxy ? proxy->GetRaw() : NULL;
	if (!raw)
		return NULL;
	ServerClass *sc = VCall<ServerClass *>(raw, vt::GetServerClass);
	int off = 0;
	SendProp *p = FindSendPropDeep(sc->m_pTable, "teamplayroundbased_gamerules_data", 0, &off);
	if (!p || !p->GetDataTableProxyFn())
		return NULL;
	CSendProxyRecipients recip;
	return (void *)p->GetDataTableProxyFn()(p, raw, (char *)raw + off, &recip, 0);
}

template <typename T>
static T GameRulesGet(const char *name, T def)
{
	void *gr = GameRulesObject();
	if (!gr)
		return def;
	static ServerClass *sc = NULL;
	if (!sc)
		for (ServerClass *c = gamedll->GetAllServerClasses(); c; c = c->m_pNext)
			if (!V_strcmp(c->GetName(), "CTFGameRulesProxy"))
				sc = c;
	if (!sc)
		return def;
	// offsets inside the gamerules data tables are relative to the gamerules object
	SendTable *t = sc->m_pTable;
	for (int i = 0; i < t->GetNumProps(); ++i)
	{
		SendProp *p = t->GetProp(i);
		if (p->GetType() != DPT_DataTable || !p->GetDataTable())
			continue;
		const char *pn = p->GetName();
		if (pn && (V_strstr(pn, "gamerules_data") || !V_strcmp(pn, "baseclass")))
		{
			int off = 0;
			if (FindSendPropDeep(p->GetDataTable(), name, 0, &off))
				return *(T *)((char *)gr + off);
		}
	}
	return def;
}

CBaseEntity *CTFGameRules::Ent() const { return gEntList.FindEntityByClassname(NULL, "ff_gamerules"); }
gamerules_roundstate_t CTFGameRules::State_Get() const { return (gamerules_roundstate_t)GameRulesGet<int>("m_iRoundState", GR_STATE_RND_RUNNING); }
bool CTFGameRules::InSetup() const { return GameRulesGet<bool>("m_bInSetup", false); }
bool CTFGameRules::IsInWaitingForPlayers() const { return GameRulesGet<bool>("m_bInWaitingForPlayers", false); }
bool CTFGameRules::InOvertime() const { return GameRulesGet<bool>("m_bInOvertime", false); }
int CTFGameRules::GetWinningTeam() const { return GameRulesGet<int>("m_iWinningTeam", 0); }
bool CTFGameRules::IsInKothMode() const { return GameRulesGet<bool>("m_bPlayingKoth", false); }
bool CTFGameRules::IsCTFCPHybrid() const { return GameRulesGet<bool>("m_bCTFCPHybrid", false); }
bool CTFGameRules::IsInArenaMode() const { return gEntList.FindEntityByClassname(NULL, "tf_logic_arena") != NULL; }

int CTFGameRules::GetGameType() const
{
	if (gEntList.FindEntityByClassname(NULL, "team_train_watcher"))
		return TF_GAMETYPE_ESCORT;
	if (IsInArenaMode())
		return TF_GAMETYPE_ARENA;
	int t = GameRulesGet<int>("m_nGameType", TF_GAMETYPE_UNDEFINED);
	if ((t == TF_GAMETYPE_CTF && PF2_FlagsAreObjective()) || t == TF_GAMETYPE_CP)
		return t;
	if (PF2_FlagsAreObjective())
		return TF_GAMETYPE_CTF;
	if (gEntList.FindEntityByClassname(NULL, "team_control_point"))
		return TF_GAMETYPE_CP;
	return TF_GAMETYPE_UNDEFINED;
}

// PF2NB: a flag only counts as an objective when the map has somewhere to capture it
// (turkeyburgers_tfc's "flag" is a soccer ball with no capture zone)
bool PF2_FlagsAreObjective()
{
	return ICaptureFlagAutoList::AutoList().Count() > 0 && ICaptureZoneAutoList::AutoList().Count() > 0;   // FFNB: census
}

// PF2NB: no flag, control point or payload on the map (dm_/tdm_/mulch_dm/MARIO_KART/turkeyburgers/
// wacky_races ...): bots roam instead of looking for an objective. Cached per map for a few seconds.
bool PF2_IsObjectivelessMap()
{
	static float s_until = -1.0f;
	static bool s_result = false;
	if (gpGlobals->curtime < s_until && gpGlobals->curtime > s_until - 10.0f)
		return s_result;
	s_until = gpGlobals->curtime + 5.0f;
	s_result = !gEntList.FindEntityByClassname(NULL, "team_train_watcher") &&
		!PF2_FlagsAreObjective() &&
		!gEntList.FindEntityByClassname(NULL, "team_control_point");
	return s_result;
}

bool CTFGameRules::IsAttackDefenseMode()
{
	// TF2: CP map where one team owns every point at round start. PF2 flag A/D uses flags with
	// m_nGameType == TF_FLAGTYPE_ATTACK_DEFEND, handled by the CTF behaviours.
	CTFObjectiveResource *res = TFObjectiveResource();
	int n = res->GetNumControlPoints();
	if (n <= 0)
		return false;
	int owner = res->GetOwningTeam(0);
	if (owner != TF_TEAM_RED && owner != TF_TEAM_BLUE)
		return false;
	for (int i = 1; i < n; ++i)
		if (res->GetOwningTeam(i) != owner)
			return false;
	return true;
}

bool CTFGameRules::IsHuntedOrVIPMap() const
{
	const char *map = STRING(gpGlobals->mapname);
	if (map && (V_stristr(map, "hunted") || V_stristr(map, "vip") || StartsWith(map, "hunted_")))
		return true;
	// runtime evidence: a class filter for the civilian, or a civilian-only spawn
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "filter_tf_class"); e; e = gEntList.FindEntityByClassname(e, "filter_tf_class"))
	{
		int *cls = e->Field<int>("m_iAllowedClass");
		if (cls && *cls == TF_CLASS_CIVILIAN)
			return true;
	}
	return false;
}

// FFNB: Lua team setup. m_iMaxPlayers -1 = team not used on this map; m_iClasses[class] is the
// class limit (-1 not allowed, 0 unlimited, n = at most n)
bool FF_TeamIsActive(int team)
{
	CBaseEntity *t = GetGlobalTeam(team);
	int *p = t ? t->SendField<int>("m_iMaxPlayers") : NULL;
	return t && (!p || *p != -1);
}

int FF_TeamClassLimit(int team, int tfclass)
{
	CBaseEntity *t = GetGlobalTeam(team);
	return t ? SendArrayGet<int>(t, "m_iClasses", tfclass, 0) : 0;
}

int FF_TeamClassCount(int team, int tfclass)
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CTFPlayer *p = ToTFPlayer(UTIL_PlayerByIndex(i));
		if (p && p->GetTeamNumber() == team && p->GetDesiredPlayerClassIndex() == tfclass)
			++n;
	}
	return n;
}

CON_COMMAND(ffnb_teams, "FF team setup as the bots see it: active, player limit, allies, FFA, class limits")
{
	for (int t = FIRST_GAME_TEAM; t < TF_TEAM_COUNT; ++t)
	{
		CTFTeam *team = GetGlobalTFTeam(t);
		if (!team)
			continue;
		int *maxp = team->SendField<int>("m_iMaxPlayers");
		char limits[128] = "";
		for (int c = 1; c <= 10; ++c)
			V_strncat(limits, UTIL_VarArgs(" %d", FF_TeamClassLimit(t, c)), sizeof(limits));
		Msg("[FFNB] team %d: players %d active=%d max=%d allies=0x%x ffa=%d friendly-to:%s%s%s%s classes:%s",
			t, team->GetNumPlayers(), FF_TeamIsActive(t) ? 1 : 0, maxp ? *maxp : -99, team->GetAllies(), team->IsFFA() ? 1 : 0,
			FF_AreTeamsFriendly(t, 2) ? " 2" : "", FF_AreTeamsFriendly(t, 3) ? " 3" : "", FF_AreTeamsFriendly(t, 4) ? " 4" : "",
			FF_AreTeamsFriendly(t, 5) ? " 5" : "", limits);
		Msg("\n");
	}
}

bool CTFGameRules::CanBotChooseClass(CBasePlayer *pBot, int iDesiredClassIndex)
{
	extern bool PF2NB_ClassAllowedForTeam(int team, int tfclass);
	return PF2NB_ClassAllowedForTeam(pBot ? pBot->GetTeamNumber() : 0, iDesiredClassIndex);
}

// CTeamplayRules::WouldChangeUnbalanceTeams semantics: joining iNewTeam (from iCurrentTeam) is
// unbalancing if it puts iNewTeam more than mp_teams_unbalance_limit players ahead of the other team.
bool CTFGameRules::WouldChangeUnbalanceTeams(int iNewTeam, int iCurrentTeam)
{
	if (iNewTeam != TF_TEAM_RED && iNewTeam != TF_TEAM_BLUE)
		return false;
	if (iNewTeam == iCurrentTeam)
		return false;
	static ConVarRef limitRef("mp_teams_unbalance_limit", true);
	int limit = limitRef.IsValid() ? limitRef.GetInt() : 1;
	if (limit <= 0)
		return false;
	int other = iNewTeam == TF_TEAM_RED ? TF_TEAM_BLUE : TF_TEAM_RED;
	CTeam *newTeam = GetGlobalTeam(iNewTeam), *otherTeam = GetGlobalTeam(other);
	int newCount = newTeam ? newTeam->GetNumPlayers() : 0;
	int otherCount = otherTeam ? otherTeam->GetNumPlayers() : 0;
	if (iCurrentTeam == other)
		--otherCount;
	return (newCount + 1) - otherCount > limit;
}

CTeamRoundTimer *CTFGameRules::GetActiveRoundTimer()
{
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "team_round_timer"); e; e = gEntList.FindEntityByClassname(e, "team_round_timer"))
	{
		CTeamRoundTimer *t = static_cast<CTeamRoundTimer *>(e);
		if (t->ShowInHud() && !t->IsTimerPaused())
			return t;
	}
	return NULL;
}

float CTeamRoundTimer::GetTimeRemaining()
{
	float *end = Field<float>("m_flTimerEndTime");
	bool *paused = Field<bool>("m_bTimerPaused");
	float *rem = Field<float>("m_flTimeRemaining");
	if (paused && *paused && rem)
		return *rem;
	return end ? MAX(0.0f, *end - gpGlobals->curtime) : 0.0f;
}

void CTFGameRules::CollectCapturePoints(CBasePlayer *player, CUtlVector<CTeamControlPoint *> *captureVector) const
{
	if (!captureVector)
		return;
	captureVector->RemoveAll();
	CTFObjectiveResource *res = TFObjectiveResource();
	CTeamControlPointMaster master;
	int n = res->GetNumControlPoints();
	if (IsInKothMode() && n == 1)
	{
		if (CTeamControlPoint *p = master.GetControlPoint(0))
			captureVector->AddToTail(p);
		return;
	}
	for (int i = 0; i < n; ++i)
	{
		CTeamControlPoint *point = master.GetControlPoint(i);
		if (!point || !res->IsCPVisible(i))
			continue;
		if (res->GetOwningTeam(i) == player->GetTeamNumber())
			continue;
		if (res->TeamCanCapPoint(i, player->GetTeamNumber()) && !point->IsLocked())
			captureVector->AddToTail(point);
	}
}

void CTFGameRules::CollectDefendPoints(CBasePlayer *player, CUtlVector<CTeamControlPoint *> *defendVector) const
{
	if (!defendVector)
		return;
	defendVector->RemoveAll();
	CTFObjectiveResource *res = TFObjectiveResource();
	CTeamControlPointMaster master;
	int enemy = player->GetTeamNumber() == TF_TEAM_RED ? TF_TEAM_BLUE : TF_TEAM_RED;
	for (int i = 0; i < res->GetNumControlPoints(); ++i)
	{
		CTeamControlPoint *point = master.GetControlPoint(i);
		if (!point || !res->IsCPVisible(i))
			continue;
		if (res->GetOwningTeam(i) != player->GetTeamNumber())
			continue;
		if (res->TeamCanCapPoint(i, enemy) && !point->IsLocked())
			defendVector->AddToTail(point);
	}
}

static CTeamTrainWatcher *FindWatcher(int team, bool exactTeam)
{
	CTeamTrainWatcher *any = NULL;
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "team_train_watcher"); e; e = gEntList.FindEntityByClassname(e, "team_train_watcher"))
	{
		CTeamTrainWatcher *w = static_cast<CTeamTrainWatcher *>(e);
		if (w->IsDisabled())
			continue;
		if (w->GetTeamNumber() == team)
			return w;
		if (!any)
			any = w;
	}
	return exactTeam ? NULL : any;
}

bool CTFGameRules::HasMultipleTrains() const
{
	// PF2NB: PF2 has no m_bIsPayloadRace; team_train_watcher sets m_bMultipleTrains for race maps.
	// Fall back to "an enabled watcher for each team".
	if (GameRulesGet<bool>("m_bMultipleTrains", false))
		return true;
	return FindWatcher(TF_TEAM_RED, true) && FindWatcher(TF_TEAM_BLUE, true);
}

CTeamTrainWatcher *CTFGameRules::GetPayloadToPush(int pushingTeam) const
{
	if (GetGameType() != TF_GAMETYPE_ESCORT)
		return NULL;
	bool race = HasMultipleTrains();
	if (race)
		return FindWatcher(pushingTeam, true);
	return pushingTeam == TF_TEAM_BLUE ? FindWatcher(TF_TEAM_BLUE, false) : NULL;
}

CTeamTrainWatcher *CTFGameRules::GetPayloadToBlock(int blockingTeam) const
{
	if (GetGameType() != TF_GAMETYPE_ESCORT)
		return NULL;
	bool race = HasMultipleTrains();
	if (race)
		return FindWatcher(blockingTeam == TF_TEAM_RED ? TF_TEAM_BLUE : TF_TEAM_RED, true);
	return blockingTeam == TF_TEAM_RED ? FindWatcher(TF_TEAM_BLUE, false) : NULL;
}

static void RefreshItemVectors(CTFGameRules *gr)
{
	if (gr->m_itemVectorTime >= 0 && gpGlobals->curtime - gr->m_itemVectorTime < 1.0f && gpGlobals->curtime >= gr->m_itemVectorTime)
		return;
	gr->m_itemVectorTime = gpGlobals->curtime;
	gr->m_healthVector.RemoveAll();
	gr->m_ammoVector.RemoveAll();
	gr->m_armorVector.RemoveAll();
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "item_*"); e; e = gEntList.FindEntityByClassname(e, "item_*"))
	{
		if (e->ClassMatches("item_healthkit*") || e->ClassMatches("item_healthammokit*"))
			gr->m_healthVector.AddToTail(e);
		if (e->ClassMatches("item_ammopack*") || e->ClassMatches("item_healthammokit*") || e->ClassMatches("item_backpack*"))
			gr->m_ammoVector.AddToTail(e);
		if (e->ClassMatches("item_armor*"))		// PF2 armor pickup (CArmor): +200 up to the class max
			gr->m_armorVector.AddToTail(e);
	}
	for (CBaseEntity *e = gEntList.FindEntityByClassname(NULL, "func_regenerate"); e; e = gEntList.FindEntityByClassname(e, "func_regenerate"))
	{
		gr->m_healthVector.AddToTail(e);
		gr->m_ammoVector.AddToTail(e);
		gr->m_armorVector.AddToTail(e);	// resupply also refills armor
	}
	// FFNB: FF's pickups are Lua backpacks (info_ff_script), with their contents from the census.
	// A taken pack is hidden / removed until it respawns.
	for (int i = 0; i < ff::Census_Count(); ++i)
	{
		const ff::CensusObj *o = ff::Census_At(i);
		if (o->kind != ff::OBJ_BACKPACK)
			continue;
		CBaseEntity *e = PF2_EntityByIndex(o->ent);
		if (!e || e->IsEffectActive(EF_NODRAW))
			continue;
		int *pos = e->SendField<int>("m_iPosState");
		if (pos && (*pos & 0xF) == 3)
			continue;
		if (o->packHealth > 0)
			gr->m_healthVector.AddToTail(e);
		if (o->packAmmo > 0 || o->packGrenades > 0)
			gr->m_ammoVector.AddToTail(e);
		if (o->packArmor > 0)
			gr->m_armorVector.AddToTail(e);
	}
}

CUtlVector<CHandle<CBaseEntity> > &CTFGameRules::GetHealthEntityVector() { RefreshItemVectors(this); return m_healthVector; }
CUtlVector<CHandle<CBaseEntity> > &CTFGameRules::GetAmmoEntityVector() { RefreshItemVectors(this); return m_ammoVector; }
CUtlVector<CHandle<CBaseEntity> > &CTFGameRules::GetArmorEntityVector() { RefreshItemVectors(this); return m_armorVector; }
void CTFGameRules::OnLevelInit() { m_itemVectorTime = -1.0f; m_healthVector.RemoveAll(); m_ammoVector.RemoveAll(); m_armorVector.RemoveAll(); }

// ---------------------------------------------------------------------------------------------
// Objective resource
// ---------------------------------------------------------------------------------------------
static CTFObjectiveResource s_objRes;
CTFObjectiveResource *TFObjectiveResource() { return &s_objRes; }
CBaseEntity *CTFObjectiveResource::Ent() { return gEntList.FindEntityByClassname(NULL, "tf_objective_resource"); }
int CTFObjectiveResource::GetNumControlPoints() { CBaseEntity *e = Ent(); int *p = e ? e->Field<int>("m_iNumControlPoints") : NULL; return p ? *p : 0; }
int CTFObjectiveResource::GetOwningTeam(int i) { return SendArrayGet<int>(Ent(), "m_iOwner", i, 0); }
bool CTFObjectiveResource::IsCPVisible(int i) { return SendArrayGet<int>(Ent(), "m_bCPIsVisible", i, 1) != 0; }
int CTFObjectiveResource::GetCappingTeam(int i) { return SendArrayGet<int>(Ent(), "m_iCappingTeam", i, 0); }
float CTFObjectiveResource::GetCPCapPercentage(int i) { return SendArrayGet<float>(Ent(), "m_flLazyCapPerc", i, 0.0f); }

bool CTFObjectiveResource::TeamCanCapPoint(int i, int team)
{
	// m_bTeamCanCap is laid out [point + team * MAX_CONTROL_POINTS] (bool elements)
	return SendArrayGet<bool>(Ent(), "m_bTeamCanCap", i + team * MAX_CONTROL_POINTS, true);
}

int CTFObjectiveResource::GetPreviousPointForPoint(int index, int team, int iPrevIndex)
{
	return SendArrayGet<int>(Ent(), "m_iPreviousPoints", (index * MAX_CONTROL_POINT_TEAMS + team) * MAX_PREVIOUS_POINTS + iPrevIndex, -1);
}
