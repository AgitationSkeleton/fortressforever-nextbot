// ff-nextbot: the map's Lua objectives as reported by maps/globalscripts/ffnb_botinfo.lua.
//
// The census script walks the map's Lua globals at startup() and sends one pair of "luaevent"
// game events per objective entity (ffnb_obj: name + metatable ancestry + entity class;
// ffnb_objf: team|botgoaltype + accepted item list), then ffnb_census_done. Runtime state changes
// (cap enabled/disabled, phase) arrive as ffnb_state events. This module turns them into a table
// keyed by entity index that the proxy factory and the bot behaviours query.
#pragma once

namespace ff {

enum ObjKind
{
	OBJ_NONE = 0,
	OBJ_FLAG,           // carried objective (baseflag, base_id_flag, ...)
	OBJ_CAP,            // capture trigger (basecap, base_id_cap, ...)
	OBJ_BALL,           // ball (fortball, murderball)
	OBJ_BACKPACK,       // genericbackpack: health/ammo/armor/grenades
	OBJ_HUNTED_ESCAPE,
	OBJ_RESPAWNDOOR,    // team-only spawn door trigger
	OBJ_CPZONE,         // command point zone (base_cp): stand in it to capture, owner from Lua
	OBJ_SECURITY,       // base_shutdown security switch (button_<team> or <team>_security_trigger); owner = the team
	                    // whose security it turns off (lasers, flag room doors), touched/pressed by its enemies
	OBJ_OTHER,
};

// Omni-bot goal types as set by map Lua (Bot.kFlag ...)
enum { GOAL_NONE = 0, GOAL_AMMO, GOAL_ARMOR, GOAL_HEALTH, GOAL_GRENADES, GOAL_FLAG, GOAL_FLAGCAP, GOAL_TRAINERSPAWN, GOAL_HUNTEDESCAPE };

struct CensusObj
{
	int  ent;
	char name[64];        // Lua table name == entity targetname
	char base[160];       // "baseflag<info_ff_script<baseclass"
	char cls[32];         // entity classname
	int  team;            // Lua "team" field, -1 if none
	int  goal;            // botgoaltype, 0 if none
	char items[160];      // caps: accepted item names, comma separated
	ObjKind kind;
	bool disabled;        // from ffnb_state (Lua Remove/Restore), default false
	int  touchMask;       // teams allowed to touch it (1 << team), -1 = any
	int  owner;           // command points: owning (defending) team, 0 = neutral, -1 unknown; security: whose
	bool detTarget;       // trigger with its own Lua onexplode (detpack walls, command centres): detpack it
	int  spareTeam;       // Lua filter not_<team>_trigger: the one team it does not act on, -1 if none
	int  packHealth, packArmor, packAmmo, packGrenades;   // backpacks only
};

void Census_Register();     // add the luaevent listener (plugin load)
void Census_Unregister();
void Census_Reset();        // level shutdown / census re-run
void Census_EnsureScript(); // sv_globalluascript ffnb_botinfo
bool Census_Done();
int  Census_Count();
const CensusObj *Census_At(int i);
const CensusObj *Census_Get(int entindex);   // NULL if the census does not know this entity
int  Census_Team(int entindex);             // Lua team of a census object, -1 if unknown
bool Census_CapAccepts(const CensusObj *cap, const char *itemName);
const char *Census_KindName(ObjKind k);
bool Census_SecurityKnown(int team);        // the map has a base_shutdown security system for this team
bool Census_SecurityUp(int team);           // that system is currently on (lasers up, flag room shut)
int  Census_SpareTeam(int entindex);        // trigger whose Lua filter spares one team (not_<team>_trigger): that team, else -1

} // namespace ff
