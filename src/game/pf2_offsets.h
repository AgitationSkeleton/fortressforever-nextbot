// ff-nextbot: Windows vtable slots in Fortress Forever 2013's server.dll (CFFPlayer and its bases).
// Derived from FF's unstripped Linux server.so (2.0.0.5, 2025-05-26) with tools/vtdump.py
// (Windows = Linux - 1). The plugin validates the PlayerRunCommand hook at runtime and refuses to
// drive bots if it never fires. (File name kept from pf2-nextbot to limit churn.)
#pragma once

namespace pf2 { namespace vt {
enum
{
	GetRefEHandle        = 2,
	GetCollideable       = 3,
	GetServerClass       = 9,
	GetDataDescMap       = 11,
	ShouldCollide        = 16,   // CBaseEntity::ShouldCollide(int collisionGroup, int contentsMask) const
	Spawn                = 24,   // CFFPlayer::Spawn
	Classify             = 58,
	IsAlive              = 70,
	MyCombatCharacter    = 76,
	IsPlayer             = 86,
	IsBaseObject         = 89,
	Teleport             = 115,
	GetMaxHealth         = 124,
	EyePosition          = 139,
	EyeAngles            = 140,
	WorldSpaceCenter     = 158,
	Weapon_Switch        = 273,
	Weapon_ShootPosition = 274,
	Weapon_CanSwitchTo   = 275,
	Weapon_GetSlot       = 277,
	ForceRespawn         = 334,   // CBasePlayer::ForceRespawn
	GetPlayerMins        = 347,
	GetPlayerMaxs        = 348,
	PlayerRunCommand     = 428,
	// CommitSuicide is overloaded (Linux 449/450); MSVC may order overloads differently, so FF
	// bots suicide with the "kill" client command instead of a vtable call.
};
} }
