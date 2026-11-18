// ff-nextbot: Fortress Forever weapons as TFBot sees them.
//
// TFBot reasons in TF weapon ids and in three slots (primary 0, secondary 1, melee 2). FF has its
// own weapon classes and buckets, so each FF weapon classname maps to the closest TF weapon id
// (behaviour: hitscan vs projectile vs melee vs flame), a TFBot slot, a rank within that slot
// (higher wins when a class owns several) and the projectile speed from FF's weapon scripts.
#pragma once

namespace ff {

struct WeaponInfo
{
	const char *classname;   // "ff_weapon_rpg"
	int   tfId;              // TF_WEAPON_* equivalent
	int   tfSlot;            // 0 primary, 1 secondary, 2 melee, -1 not a combat weapon
	int   rank;
	float projectileSpeed;   // 0 = hitscan / melee
};

const WeaponInfo *WeaponByClassname(const char *classname);   // NULL if unknown

} // namespace ff
