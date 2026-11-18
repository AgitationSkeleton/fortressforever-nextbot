// ff-nextbot: FF weapon table (see ff_weapons.h). Speeds are the "Speed" keys of FF 2.0.0.5's
// scripts/ff_weapon_*.txt.
#include "cbase.h"
#include <string.h>
#include "ff_weapons.h"


namespace ff {

static const WeaponInfo s_weapons[] =
{
	// primaries (a class's main weapon)
	{ "ff_weapon_rpg",            TF_WEAPON_ROCKETLAUNCHER,    0, 90, 1000.0f },
	{ "ff_weapon_assaultcannon",  TF_WEAPON_MINIGUN,           0, 90, 0.0f },
	{ "ff_weapon_flamethrower",   TF_WEAPON_FLAMETHROWER,      0, 90, 0.0f },
	{ "ff_weapon_grenadelauncher",TF_WEAPON_GRENADELAUNCHER,   0, 90, 625.0f },
	{ "ff_weapon_sniperrifle",    TF_WEAPON_SNIPERRIFLE,       0, 90, 0.0f },
	{ "ff_weapon_supernailgun",   TF_WEAPON_NAILGUN,           0, 80, 2000.0f },
	{ "ff_weapon_nailgun",        TF_WEAPON_NAILGUN,           0, 60, 2000.0f },
	{ "ff_weapon_supershotgun",   TF_WEAPON_SHOTGUN_PRIMARY,   0, 50, 0.0f },
	// secondaries
	{ "ff_weapon_pipelauncher",   TF_WEAPON_PIPEBOMBLAUNCHER,  1, 90, 625.0f },
	{ "ff_weapon_ic",             TF_WEAPON_FLAMETHROWER_ROCKET, 1, 85, 1000.0f },
	{ "ff_weapon_autorifle",      TF_WEAPON_SMG,               1, 80, 0.0f },
	{ "ff_weapon_tranq",          TF_WEAPON_TRANQ,             1, 75, 1500.0f },
	{ "ff_weapon_railgun",        TF_WEAPON_NAILGUN,           1, 70, 1500.0f },
	{ "ff_weapon_tommygun",       TF_WEAPON_SMG,               1, 60, 0.0f },
	{ "ff_weapon_shotgun",        TF_WEAPON_SHOTGUN_SOLDIER,   1, 50, 0.0f },
	// melee
	{ "ff_weapon_medkit",         TF_WEAPON_SYRINGE,           2, 90, 0.0f },   // heals/cures allies, infects enemies (as PF2's syringe)
	{ "ff_weapon_spanner",        TF_WEAPON_WRENCH,            2, 90, 0.0f },
	{ "ff_weapon_knife",          TF_WEAPON_KNIFE,             2, 90, 0.0f },
	{ "ff_weapon_umbrella",       TF_WEAPON_UMBRELLA,          2, 80, 0.0f },
	{ "ff_weapon_crowbar",        TF_WEAPON_CROWBAR,           2, 50, 0.0f },
	// not combat weapons: building is done with client commands, the jump gun is a movement tool
	{ "ff_weapon_deploysentrygun", TF_WEAPON_BUILDER,          -1, 0, 0.0f },
	{ "ff_weapon_deploydispenser", TF_WEAPON_BUILDER,          -1, 0, 0.0f },
	{ "ff_weapon_deploydetpack",   TF_WEAPON_BUILDER,          -1, 0, 0.0f },
	{ "ff_weapon_deploymancannon", TF_WEAPON_BUILDER,          -1, 0, 0.0f },
	{ "ff_weapon_jumpgun",         TF_WEAPON_NONE,             -1, 0, 0.0f },
	{ "ff_weapon_flag",            TF_WEAPON_NONE,             -1, 0, 0.0f },
};

const WeaponInfo *WeaponByClassname(const char *classname)
{
	if (!classname)
		return NULL;
	for (const WeaponInfo &w : s_weapons)
		if (!strcmp(w.classname, classname))
			return &w;
	return NULL;
}

} // namespace ff
