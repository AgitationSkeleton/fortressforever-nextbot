//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_fetch_flag.cpp
// Go get the flag!
// Michael Booth, May 2011

#include "cbase.h"

#include "bot/tf_bot.h"
#include "bot/behavior/scenario/capture_the_flag/tf_bot_fetch_flag.h"
#include "bot/behavior/scenario/capture_the_flag/tf_bot_escort_flag_carrier.h"
#include "bot/behavior/scenario/capture_the_flag/tf_bot_attack_flag_defenders.h"
#include "bot/behavior/scenario/capture_the_flag/tf_bot_deliver_flag.h"
#include "behaviors/ff_disable_security.h"   // FFNB
#include "behaviors/ff_jump_pad.h"           // FFNB
#include "bot/behavior/tf_bot_seek_and_destroy.h"   // FFNB


//---------------------------------------------------------------------------------------------
CTFBotFetchFlag::CTFBotFetchFlag( bool isTemporary )
{
	m_isTemporary = isTemporary;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotFetchFlag::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );

	return Continue();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotFetchFlag::Update( CTFBot *me, float interval )
{
	CCaptureFlag *flag = me->GetFlagToFetch();

	if ( !flag )
	{
		if ( TFGameRules()->IsMannVsMachineMode() )
		{
			return SuspendFor( new CTFBotAttackFlagDefenders, "Flag flag exists - Attacking the enemy flag defenders" );
		}

		return Done( "No flag" );
	}

	// uncloak so we can attack
	if ( me->m_Shared.IsStealthed() )
	{
		me->PressAltFireButton();
	}

	if ( TFGameRules()->IsMannVsMachineMode() && flag->IsHome() )
	{
		if ( gpGlobals->curtime - me->GetSpawnTime() < 1.0f && me->GetTeamNumber() != TEAM_SPECTATOR )
		{
			// we just spawned - give us the flag
			flag->PickUp( me, true );
		}
		else
		{
			if ( m_isTemporary )
			{
				return Done( "Flag unreachable" );
			}

			// flag is at home and we're out in the world - can't reach it
			return SuspendFor( new CTFBotAttackFlagDefenders, "Flag unreachable at home - Attacking the enemy flag defenders" );
		}
	}

	const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
	if ( threat )
	{
		me->EquipBestWeaponForThreat( threat );
	}

	CTFPlayer *carrier = ToTFPlayer( flag->GetOwnerEntity() );
	if ( carrier )
	{
		if ( m_isTemporary )
		{
			return Done( "Someone else picked up the flag" );
		}

		// NOTE: if I've picked up the flag, the ScenarioMonitor will handle it
		return SuspendFor( new CTFBotAttackFlagDefenders, "Someone has the flag - attacking the enemy defenders" );
	}

	// FFNB: base_shutdown maps keep a flag at home behind security (lasers, flag room door) that its
	// enemies must turn off first; go do that, then come back for the flag while it is down
	if ( flag->IsHome() && m_securityRetryTimer.IsElapsed() && CTFBotFFDisableSecurity::IsNeeded( me, flag->GetTeamNumber() ) )
	{
		m_securityRetryTimer.Start( 15.0f );
		return SuspendFor( new CTFBotFFDisableSecurity( flag->GetTeamNumber() ), "Enemy security is up - turning it off" );
	}

	// FFNB: scouts build a jump pad when a simulated launch lands them well along the way to the flag
	if ( me->IsPlayerClass( TF_CLASS_SCOUT ) && m_jumpPadTimer.IsElapsed() )
	{
		m_jumpPadTimer.Start( RandomFloat( 8.0f, 15.0f ) );
		Vector padSpot;
		float padYaw;
		if ( CTFBotFFJumpPad::FindLaunch( me, flag->WorldSpaceCenter(), &padSpot, &padYaw ) )
		{
			return SuspendFor( new CTFBotFFJumpPad( padSpot, padYaw ), "Building a jump pad toward the flag" );
		}
	}

	// go pick up the flag
	if ( m_repathTimer.IsElapsed() )
	{
		CTFBotPathCost cost( me, DEFAULT_ROUTE );
		float maxPathLength = TFGameRules()->IsMannVsMachineMode() ? TFBOT_MVM_MAX_PATH_LENGTH : 0.0f;
		bool computed = m_path.Compute( me, flag->WorldSpaceCenter(), cost, maxPathLength );
		// FFNB: Compute() also fails when the flag is just outside the closest area, so judge by where the
		// path ends. A flag at home that no path reaches (a flag room the mesh doesn't cover) used to leave
		// the bot standing at the end of its partial path for good; every bot on the team ended up there
		if ( !flag->IsDropped() && ( !m_path.IsValid() || ( m_path.GetEndPosition() - flag->GetAbsOrigin() ).IsLengthGreaterThan( 200.0f ) ) )
		{
			if ( m_isTemporary )
			{
				return Done( "Flag unreachable" );
			}
			m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );
			return SuspendFor( new CTFBotSeekAndDestroy( RandomFloat( 10.0f, 20.0f ) ), "Flag unreachable at home - hunting enemies" );
		}
		if ( computed == false )
		{
			if ( flag->IsDropped() )
			{
				// flag is unreachable - attack for awhile and hope someone else can dislodge it
				return SuspendFor( new CTFBotAttackFlagDefenders( RandomFloat( 5.0f, 10.0f ) ), "Flag unreachable - Attacking" );

				// just give it to me
				// flag->PickUp( me, true );
			}
		}

		m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );
	}

	m_path.Update( me );

	return Continue();
}


//---------------------------------------------------------------------------------------------
// are we in a hurry?
QueryResultType CTFBotFetchFlag::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}


//---------------------------------------------------------------------------------------------
// is it time to retreat?
QueryResultType	CTFBotFetchFlag::ShouldRetreat( const INextBot *me ) const
{
	return ANSWER_NO;
}
