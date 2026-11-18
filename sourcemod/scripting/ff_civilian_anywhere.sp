/**
 * ff_civilian_anywhere - let players (and bots) pick the Civilian on every Fortress Forever map,
 * just for fun, except on maps where the Civilian has a special role (Hunted VIP) or that you list.
 *
 * FF keeps each team's class limits in CFFTeam::m_iClasses (networked; -1 = not allowed,
 * 0 = unlimited, n = at most n); the map's Lua sets them (SetClassLimit). This plugin changes only
 * the Civilian entry (class 10) of active teams from -1 to sm_civanywhere_limit, re-applying it
 * whenever the map script resets the limits. It replaces the old approach of overriding every map
 * script with modified copies (custom/civ), so map scripts stay stock.
 */
#include <sourcemod>
#include <sdktools>

#pragma semicolon 1
#pragma newdecls required

#define CLASS_CIVILIAN 10

public Plugin myinfo =
{
	name = "FF Civilian Anywhere",
	author = "ff-nextbot contributors",
	description = "Allow the Civilian class on every map except excluded ones",
	version = "1.0.0",
	url = ""
};

ConVar g_cvEnabled;
ConVar g_cvExclude;
ConVar g_cvLimit;
bool g_excluded;

public void OnPluginStart()
{
	g_cvEnabled = CreateConVar("sm_civanywhere_enabled", "1", "Allow the Civilian on every map (1) or leave the map's class limits alone (0)", _, true, 0.0, true, 1.0);
	g_cvExclude = CreateConVar("sm_civanywhere_exclude", "ff_hunted*,hunted*,ff_training",
		"Comma-separated map names (trailing * = prefix) where the map's own Civilian rule is kept");
	g_cvLimit = CreateConVar("sm_civanywhere_limit", "0", "Civilian limit per team where it is allowed by this plugin (0 = unlimited)", _, true, 0.0);
	AutoExecConfig(true, "ff_civilian_anywhere");
	HookConVarChange(g_cvExclude, OnExcludeChanged);
	CreateTimer(2.0, Timer_Apply, _, TIMER_REPEAT);
}

public void OnMapStart()
{
	g_excluded = MapExcluded();
}

public void OnExcludeChanged(ConVar cv, const char[] oldValue, const char[] newValue)
{
	g_excluded = MapExcluded();
}

bool MapExcluded()
{
	char map[96], list[512], parts[32][96];
	GetCurrentMap(map, sizeof(map));
	g_cvExclude.GetString(list, sizeof(list));
	int n = ExplodeString(list, ",", parts, sizeof(parts), sizeof(parts[]));
	for (int i = 0; i < n; i++)
	{
		TrimString(parts[i]);
		int len = strlen(parts[i]);
		if (len == 0)
			continue;
		if (parts[i][len - 1] == '*')
		{
			parts[i][len - 1] = '\0';
			if (StrContains(map, parts[i], false) == 0)
				return true;
		}
		else if (StrEqual(map, parts[i], false))
		{
			return true;
		}
	}
	return false;
}

int FindDataMapOrSendOffset(int ent)
{
	char cls[64];
	if (!GetEntityNetClass(ent, cls, sizeof(cls)))
		return -1;
	return FindSendPropInfo(cls, "m_iClasses");
}

public Action Timer_Apply(Handle timer)
{
	if (!g_cvEnabled.BoolValue || g_excluded)
		return Plugin_Continue;
	int ent = -1;
	while ((ent = FindEntityByClassname(ent, "ff_team_manager")) != -1)
	{
		int team = GetEntProp(ent, Prop_Send, "m_iTeamNum");
		if (team < 2 || team > 5)
			continue;
		if (GetEntProp(ent, Prop_Send, "m_iMaxPlayers") == -1)
			continue;   // team not used on this map
		// m_iClasses is networked as 4-bit elements, so Get/SetEntProp would touch one byte of the
		// 32-bit server field; read and write the whole int at the array element's offset instead
		int off = FindDataMapOrSendOffset(ent);
		if (off <= 0)
			continue;
		off += 4 * CLASS_CIVILIAN;
		int lim = GetEntData(ent, off, 4);
		if (lim < 0)   // -1 (or a value an older build of this plugin corrupted)
			SetEntData(ent, off, g_cvLimit.IntValue, 4, true);
	}
	return Plugin_Continue;
}
