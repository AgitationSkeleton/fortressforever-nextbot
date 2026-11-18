//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_locomotion.cpp
// Team Fortress NextBot locomotion interface
// Michael Booth, May 2010

#include "cbase.h"

#include "tf_bot.h"
#include "tf_bot_locomotion.h"
#include "particle_parse.h"


//-----------------------------------------------------------------------------------------
void CTFBotLocomotion::Update( void )
{
	BaseClass::Update();

	CTFBot *me = ToTFBot( GetBot()->GetEntity() );
	if ( !me )
	{
		return;
	}

	// always 'crouch jump'
	if ( IsOnGround() )
	{
		if ( !me->IsPlayerClass( TF_CLASS_ENGINEER ) )
		{
			// engineers need to crouch behind their guns
			me->ReleaseCrouchButton();
		}
	}
	else
	{
		me->PressCrouchButton( 0.3f );
	}
}


//-----------------------------------------------------------------------------------------
// Move directly towards the given position
void CTFBotLocomotion::Approach( const Vector &pos, float goalWeight )
{
	if ( TFGameRules()->IsMannVsMachineMode() )
	{
		if ( !IsOnGround() && !IsClimbingOrJumping() )
		{
			// no air control
			return;
		}
	}

	BaseClass::Approach( pos, goalWeight );
}


//-----------------------------------------------------------------------------------------
// Distance at which we will die if we fall
float CTFBotLocomotion::GetDeathDropHeight( void ) const
{
	return 1000.0f;
}


//-----------------------------------------------------------------------------------------
// Get maximum running speed
float CTFBotLocomotion::GetRunSpeed( void ) const
{
	CTFBot *me = (CTFBot *)GetBot()->GetEntity();
	return me->GetPlayerClass()->GetMaxSpeed();
}


//-----------------------------------------------------------------------------------------
// Return true if given area can be used for navigation
bool CTFBotLocomotion::IsAreaTraversable( const CNavArea *baseArea ) const
{
	CTFBot *me = (CTFBot *)GetBot()->GetEntity();
	CTFNavArea *area = (CTFNavArea *)baseArea;

	if ( area->IsBlocked( me->GetTeamNumber() ) )
	{
		return false;
	}

	if ( !TFGameRules()->RoundHasBeenWon() || TFGameRules()->GetWinningTeam() != me->GetTeamNumber() )
	{
		// FFNB: only hostile teams' spawn rooms (Hunted: red is allied with the blue VIP), any of 4 teams
		extern bool FF_AreTeamsFriendly( int a, int b );
		if ( area->HasAttributeTF( TF_NAV_SPAWN_ROOM_RED ) && !FF_AreTeamsFriendly( me->GetTeamNumber(), TF_TEAM_RED ) )
		{
			return false;
		}

		if ( area->HasAttributeTF( TF_NAV_SPAWN_ROOM_BLUE ) && !FF_AreTeamsFriendly( me->GetTeamNumber(), TF_TEAM_BLUE ) )
		{
			return false;
		}
	}

	return true;
}


//-----------------------------------------------------------------------------------------
bool CTFBotLocomotion::IsEntityTraversable( CBaseEntity *obstacle, TraverseWhenType when ) const
{
	// assume all players are "traversable" in that they will move or can be killed
	if ( obstacle && obstacle->IsPlayer() )
	{
		return true;
	}

	// FFNB: FF team doors / clips (Lua respawndoor triggers report classname trigger_multiple;
	// trigger_ff_clip) are solid only to
	// other teams. A traced ShouldCollide has no player context, so decide here: a census object
	// with a team lets that team (and its allies) through; other FF clips are assumed passable
	// (the door-block learning handles any that are not).
	if ( obstacle && ( FClassnameIs( obstacle, "trigger_ff_script" ) || FClassnameIs( obstacle, "trigger_ff_clip" ) || FClassnameIs( obstacle, "trigger_multiple" ) ) )
	{
		extern int FF_CensusTeamOf( int entindex );
		extern bool FF_AreTeamsFriendly( int a, int b );
		int team = FF_CensusTeamOf( obstacle->entindex() );
		if ( team >= FIRST_GAME_TEAM )
			return FF_AreTeamsFriendly( GetBot()->GetEntity()->GetTeamNumber(), team );
		return true;
	}

	return PlayerLocomotion::IsEntityTraversable( obstacle, when );
}


void CTFBotLocomotion::Jump( void )
{
	BaseClass::Jump();

	CTFBot *me = ToTFBot( GetBot()->GetEntity() );
	if ( !me )
	{
		return;
	}

	if ( TFGameRules() && TFGameRules()->IsMannVsMachineMode() )
	{
		int iCustomJumpParticle = 0;
		CALL_ATTRIB_HOOK_INT_ON_OTHER( me, iCustomJumpParticle, bot_custom_jump_particle );
		if ( iCustomJumpParticle )
		{
			const char *pEffectName = "rocketjump_smoke";
			DispatchParticleEffect( pEffectName, PATTACH_POINT_FOLLOW, me, "foot_L" );
			DispatchParticleEffect( pEffectName, PATTACH_POINT_FOLLOW, me, "foot_R" );
		}
	}
}
