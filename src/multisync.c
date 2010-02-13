/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2009  Warzone Resurrection Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/*
 * MultiSync.c
 *
 * synching issues
 * This file handles the constant backstream of net info, checking that recvd info
 * is concurrent with the local world, and correcting as required. Magic happens here.
 *
 * All conflicts due to non-guaranteed messaging are detected/resolved here.
 *
 * Alex Lee, pumpkin Studios, bath.
 */

#include "lib/framework/frame.h"
#include "lib/framework/input.h"
#include "lib/framework/strres.h"

#include "stats.h"
#include "lib/gamelib/gtime.h"
#include "map.h"
#include "objects.h"
#include "display.h"								// for checking if droid in view.
#include "order.h"
#include "action.h"
#include "hci.h"									// for byte packing funcs.
#include "display3ddef.h"							// tile size constants.
#include "console.h"
#include "geometry.h"								// for gettilestructure
#include "mapgrid.h"								// for move droids directly.
#include "lib/netplay/netplay.h"
#include "multiplay.h"
#include "frontend.h"								// for titlemode
#include "multistat.h"
#include "power.h"									// for power checks
#include "multirecv.h"
#include "random.h"

// ////////////////////////////////////////////////////////////////////////////
// function definitions

static BOOL sendStructureCheck	(void);							//Structure
static PACKAGED_CHECK packageCheck(const DROID *pD);
static BOOL sendDroidCheck		(void);							//droids

static BOOL sendPowerCheck(void);
static UDWORD averagePing(void);

// ////////////////////////////////////////////////////////////////////////////
// Defined numeric values
// NOTE / FIXME: Current MP games are locked at 45ms
#define MP_FPS_LOCK			45			
#define AV_PING_FREQUENCY	MP_FPS_LOCK * 1000		// how often to update average pingtimes. in approx millisecs.
#define PING_FREQUENCY		MP_FPS_LOCK * 600		// how often to update pingtimes. in approx millisecs.
#define STRUCT_FREQUENCY	MP_FPS_LOCK * 10		// how often (ms) to send a structure check.
#define DROID_FREQUENCY		MP_FPS_LOCK * 7			// how ofter (ms) to send droid checks
#define POWER_FREQUENCY		MP_FPS_LOCK * 14		// how often to send power levels
#define SCORE_FREQUENCY		MP_FPS_LOCK * 2400		// how often to update global score.

static UDWORD				PingSend[MAX_PLAYERS];	//stores the time the ping was called.

// ////////////////////////////////////////////////////////////////////////////
// test traffic level.
static BOOL okToSend(void)
{
	// Update checks and go no further if any exceeded.
	// removing the received check again ... add NETgetRecentBytesRecvd() to left hand side of equation if this works badly
	if (NETgetRecentBytesSent() >= MAX_BYTESPERSEC)
	{
		return false;
	}

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// Droid checking info. keep position and damage in sync.
BOOL sendCheck(void)
{
	UDWORD i;

	NETgetBytesSent();			// update stats.
	NETgetBytesRecvd();
	NETgetPacketsSent();
	NETgetPacketsRecvd();

	// dont send checks till all players are present.
	for(i=0;i<MAX_PLAYERS;i++)
	{
		if(isHumanPlayer(i) && ingame.JoiningInProgress[i])
		{
			return true;
		}
	}

	// send Checks. note each send has it's own send criteria, so might not send anything.
	// Priority is droids -> structures -> power -> score -> ping

	sendDroidCheck();
	if(okToSend())
	{
		sendStructureCheck();
		sync_counter.sentStructureCheck++;

	}
	else
	{
		sync_counter.unsentStructureCheck++;
	}
	if(okToSend())
	{
		sendPowerCheck();
		sync_counter.sentPowerCheck++;
	}
	else
	{
		sync_counter.unsentPowerCheck++;
	}
	if(okToSend())
	{
		sendScoreCheck();
		sync_counter.sentScoreCheck++;
	}
	else
	{
		sync_counter.unsentScoreCheck++;
	}
	if(okToSend())
	{
		sendPing();
		sync_counter.sentPing++;
	}
	else
	{
		sync_counter.unsentPing++;
	}

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// pick a droid to send, NULL otherwise.
static DROID* pickADroid(void)
{
	DROID *pD, *ret;
	unsigned player = MAX_PLAYERS;
	unsigned i;

	// Pick a random player who has at least one droid.
	for (i = 0; i < 200; ++i)
	{
		unsigned p = gameRand(MAX_PLAYERS);
		if (apsDroidLists[p] != NULL)
		{
			player = p;
			break;
		}
	}

	if (player == MAX_PLAYERS)
	{
		return NULL;  // No players have any droids, with high probability...
	}

	// O(n) where n is number of droids. Slow, but hard to beat on a linked list. (One call of a pick n droids function would be just as fast.)
	i = 0;
	for (pD = apsDroidLists[player]; pD != NULL; pD = pD->psNext)
	{
		if (gameRand(++i) == 0)
		{
			ret = pD;
		}
	}

	return ret;
}

/** Force a droid to be synced
 *
 *  Call this when you need to update the given droid right now.
 */
BOOL ForceDroidSync(const DROID* droidToSend)
{
	uint8_t count = 1;		// *always* one
	PACKAGED_CHECK pc = packageCheck(droidToSend);

	ASSERT(droidToSend != NULL, "NULL pointer passed");

	debug(LOG_SYNC, "Force sync of droid %u from player %u", droidToSend->id, droidToSend->player);

	NETbeginEncode(NETgameQueue(selectedPlayer), GAME_CHECK_DROID);
		NETuint8_t(&count);
		NETuint32_t(&gameTime);  // Send game time.
		NETPACKAGED_CHECK(&pc);
	return NETend();
}

// ///////////////////////////////////////////////////////////////////////////
// send a droid info packet.
static BOOL sendDroidCheck(void)
{
	DROID			*pD, **ppD;
	uint8_t			i, count;
	static UDWORD	lastSent = 0;		// Last time a struct was sent.
	UDWORD			toSend = 12;

	if (lastSent > gameTime)
	{
		lastSent= 0;
	}

	// Only send a struct send if not done recently
	if (gameTime - lastSent < DROID_FREQUENCY)
	{
		return true;
	}

	lastSent = gameTime;

	NETbeginEncode(NETgameQueue(selectedPlayer), GAME_CHECK_DROID);

		// Allocate space for the list of droids to send
		ppD = alloca(sizeof(DROID *) * toSend);

		// Get the list of droids to sent
		for (i = 0, count = 0; i < toSend; i++)
		{
			pD = pickADroid();

			if (pD == NULL || (pD->gameCheckDroid != NULL && ((PACKAGED_CHECK *)pD->gameCheckDroid)->gameTime + 5000 > gameTime))
			{
				continue;  // Didn't find a droid, or droid was synched recently.
			}

			// If the droid is ours add it to the list
			if (myResponsibility(pD->player))
			{
				ppD[count++] = pD;
			}
			free(pD->gameCheckDroid);
			pD->gameCheckDroid = (PACKAGED_CHECK *)malloc(sizeof(PACKAGED_CHECK));
			*(PACKAGED_CHECK *)pD->gameCheckDroid = packageCheck(pD);
		}

		// Send the number of droids to expect
		NETuint8_t(&count);
		NETuint32_t(&gameTime);  // Send game time.

		// Add the droids to the packet
		for (i = 0; i < count; i++)
		{
			NETPACKAGED_CHECK((PACKAGED_CHECK *)ppD[i]->gameCheckDroid);
		}

	return NETend();
}

// ////////////////////////////////////////////////////////////////////////////
// Send a Single Droid Check message
static PACKAGED_CHECK packageCheck(const DROID *pD)
{
	PACKAGED_CHECK pc;
	pc.gameTime = gameTime;

	pc.player = pD->player;
	pc.droidID = pD->id;
	pc.order = pD->order;
	pc.secondaryOrder = pD->secondaryOrder;
	pc.body = pD->body;
	pc.direction = pD->direction;
	pc.experience = pD->experience;
	pc.sMoveX = pD->sMove.fx;
	pc.sMoveY = pD->sMove.fy;
	if (pD->order == DORDER_ATTACK)
	{
		pc.targetID = pD->psTarget->id;
	}
	else if (pD->order == DORDER_MOVE)
	{
		pc.orderX = pD->orderX;
		pc.orderY = pD->orderY;
	}
	return pc;
}


// ////////////////////////////////////////////////////////////////////////////
// receive a check and update the local world state accordingly
BOOL recvDroidCheck(NETQUEUE queue)
{
	uint8_t		count;
	int		i;
	uint32_t        synchTime;

	NETbeginDecode(queue, GAME_CHECK_DROID);

		// Get the number of droids to expect
		NETuint8_t(&count);
		NETuint32_t(&synchTime);  // Get game time.

		for (i = 0; i < count; i++)
		{
			DROID *         pD;
			PACKAGED_CHECK  pc, pc2;

			NETPACKAGED_CHECK(&pc);

			// Find the droid in question
			if (!IdToDroid(pc.droidID, pc.player, &pD))
			{
				NETlogEntry("Recvd Unknown droid info. val=player", 0, pc.player);
				debug(LOG_SYNC, "Received checking info for an unknown (as yet) droid. player:%d ref:%d", pc.player, pc.droidID);
				continue;
			}

			if (pD->gameCheckDroid == NULL || ((PACKAGED_CHECK *)pD->gameCheckDroid)->gameTime != synchTime)
			{
				debug(LOG_SYNC, "We got a droid %u synch, but we didn't choose the same droid to synch.", pc.droidID);
				continue;  // Can't synch, since we didn't save data to be able to calculate a delta.
			}

			pc2 = *(PACKAGED_CHECK *)pD->gameCheckDroid;

#define MERGECOPY(x, y, z1, z2)  if (pc.y != pc2.y) { debug(LOG_SYNC, "Droid %u out of synch, changing "#x" from %"z1" to %"z2".", pc.droidID, x, pc.y);             x = pc.y; }
#define MERGEDELTA(x, y, z1, z2) if (pc.y != pc2.y) { debug(LOG_SYNC, "Droid %u out of synch, changing "#x" from %"z1" to %"z2".", pc.droidID, x, x + pc.y - pc2.y); x += pc.y - pc2.y; }
			// player not synched here...
			MERGEDELTA(pD->pos.x, sMoveX, "d", "f");
			MERGEDELTA(pD->pos.y, sMoveY, "d", "f");  // Apparently the old off-screen update set both pos.[xy] and sMove.f[xy] to the received sMove.f[xy], so doing the same.
			MERGEDELTA(pD->sMove.fx, sMoveX, "f", "f");
			MERGEDELTA(pD->sMove.fy, sMoveY, "f", "f");
			MERGEDELTA(pD->direction, direction, "f", "f");
			pD->direction += pD->direction < 0 ? 360 : pD->direction >= 360 ? -360 : 0; 
			MERGEDELTA(pD->body, body, "u", "u");
			MERGEDELTA(pD->experience, experience, "f", "f");

			if (pc.sMoveX != pc2.sMoveX || pc.sMoveY != pc2.sMoveY)
			{
				// snap droid(if on ground) to terrain level at x,y.
				if ((asPropulsionStats + pD->asBits[COMP_PROPULSION].nStat)->propulsionType != PROPULSION_TYPE_LIFT)  // if not airborne.
				{
					pD->pos.z = map_Height(pD->pos.x, pD->pos.y);
				}
			}

			// Doesn't cover all cases, but at least doesn't actively break stuff randomly.
			switch (pc.order)
			{
				case DORDER_MOVE:
					if (pc.order != pc2.order || pc.orderX != pc2.orderX || pc.orderY != pc2.orderY)
					{
						debug(LOG_SYNC, "Droid %u out of synch, changing order from %s to %s(%d, %d).", pc.droidID, getDroidOrderName(pc2.order), getDroidOrderName(pc.order), pc.orderX, pc.orderY);
						// reroute the droid.
						turnOffMultiMsg(true);
						orderDroidLoc(pD, pc.order, pc.orderX, pc.orderY);
						turnOffMultiMsg(false);
					}
					break;
				case DORDER_ATTACK:
					if (pc.order != pc2.order || pc.targetID != pc2.targetID)
					{
						debug(LOG_SYNC, "Droid %u out of synch, changing order from %s to %s(%u).", pc.droidID, getDroidOrderName(pc2.order), getDroidOrderName(pc.order), pc.targetID);
						// remote droid is attacking, not here tho!
						turnOffMultiMsg(true);
						orderDroidObj(pD, pc.order, IdToPointer(pc.targetID, ANYPLAYER));
						turnOffMultiMsg(false);
					}
					break;
				case DORDER_NONE:
				case DORDER_GUARD:
					if (pc.order != pc2.order)
					{
						debug(LOG_SYNC, "Droid %u out of synch, changing order from %s to %s.", pc.droidID, getDroidOrderName(pc2.order), getDroidOrderName(pc.order));
						turnOffMultiMsg(true);
						moveStopDroid(pD);
						turnOffMultiMsg(false);
					}
					break;
				default:
					break;  // Don't know what to do, but at least won't be actively breaking anything.
			}

			MERGECOPY(pD->secondaryOrder, secondaryOrder, "u", "u");  // The old code set this after changing orders, so doing that in case.
#undef MERGECOPY
#undef MERGEDELTA

			// ...and repeat!
		}

	NETend();

	return true;
}


// ////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////
// Structure Checking, to ensure smoke and stuff is consistent across machines.
// this func is recursive!
static  STRUCTURE *pickAStructure(void)
{
	static UDWORD	player=0;					// player currently checking.
	static UDWORD	snum=0;						// structure index for this player.
	STRUCTURE		*pS=NULL;
	static UDWORD	maxtrys = 0;				// don't loop forever if failing/.
	UDWORD			i;

	if ( !myResponsibility(player) )			// dont send stuff that's not our problem.
	{
		player ++;								// next player next time.
		player = player%MAX_PLAYERS;
		snum =0;

		if(maxtrys<MAX_PLAYERS)
		{
			maxtrys ++;
			return pickAStructure();
		}
		else
		{
			maxtrys = 0;
			return NULL;
		}
	}

	pS = apsStructLists[player];				// find the strucutre
	for(i=0; ((i<snum) && (pS != NULL)) ;i++)
	{
		pS= pS->psNext;
	}


	if (pS == NULL)								// last structure or no structures at all
	{
		player ++;								// go onto the next player
		player = player%MAX_PLAYERS;
		snum=0;

		if(maxtrys<MAX_PLAYERS)
		{
			maxtrys ++;
			return pickAStructure();
		}
		else
		{
			maxtrys = 0;
			return NULL;
		}
	}
	snum ++;										// next structure next time

	maxtrys = 0;
	return pS;
}

// ////////////////////////////////////////////////////////////////////////
// Send structure information.
static BOOL sendStructureCheck(void)
{
	static UDWORD	lastSent = 0;	// Last time a struct was sent
	STRUCTURE		*pS;
    uint8_t			capacity;

	if (lastSent > gameTime)
	{
		lastSent = 0;
	}
	// Only send a struct send if not done recently
	if ((gameTime - lastSent) < STRUCT_FREQUENCY)
	{
		return true;
	}

	lastSent = gameTime;

	pS = pickAStructure();
	// Only send info about complete buildings
	if (pS && (pS->status == SS_BUILT))
	{
		NETbeginEncode(NETgameQueue(selectedPlayer), GAME_CHECK_STRUCT);
			NETuint8_t(&pS->player);
			NETuint32_t(&pS->id);
			NETuint32_t(&pS->body);
			NETuint32_t(&pS->pStructureType->ref);
			NETuint16_t(&pS->pos.x);
			NETuint16_t(&pS->pos.y);
			NETuint16_t(&pS->pos.z);
			NETfloat(&pS->direction);

			switch (pS->pStructureType->type)
			{

				case REF_RESEARCH:
					capacity = ((RESEARCH_FACILITY *) pS->pFunctionality)->capacity;
					NETuint8_t(&capacity);
					break;
				case REF_FACTORY:
				case REF_VTOL_FACTORY:
					capacity = ((FACTORY *) pS->pFunctionality)->capacity;
					NETuint8_t(&capacity);
					break;
				case REF_POWER_GEN:
					capacity = ((POWER_GEN *) pS->pFunctionality)->capacity;
					NETuint8_t(&capacity);
				default:
					break;
			}

		NETend();
	}

	return true;
}

// receive checking info about a structure and update local world state
BOOL recvStructureCheck(NETQUEUE queue)
{
	STRUCTURE		*pS;
	STRUCTURE_STATS	*psStats;
	BOOL			hasCapacity = true;
	int				i, j;
	float			direction;
	uint8_t			player, ourCapacity;
	uint32_t		body;
	uint16_t		x, y, z;
	uint32_t		ref, type;

	NETbeginDecode(queue, GAME_CHECK_STRUCT);
		NETuint8_t(&player);
		NETuint32_t(&ref);
		NETuint32_t(&body);
		NETuint32_t(&type);
		NETuint16_t(&x);
		NETuint16_t(&y);
		NETuint16_t(&z);
		NETfloat(&direction);

		if (player >= MAX_PLAYERS)
		{
			debug(LOG_ERROR, "Bad GAME_CHECK_STRUCT received!");
			NETend();
			return false;
		}

		// If the structure exists our job is easy
		pS = IdToStruct(ref, player);
		if (pS)
		{
			pS->body = body;
			pS->direction = direction;
		}
		// Structure was not found - build it
		else
		{
			NETlogEntry("scheck:structure check failed, adding struct. val=type", 0, type - REF_STRUCTURE_START);

			for (i = 0; i < numStructureStats && asStructureStats[i].ref != type; i++);
			psStats = &asStructureStats[i];

			// Check for similar buildings, to avoid overlaps
			if (TileHasStructure(mapTile(map_coord(x), map_coord(y))))
			{
				NETlogEntry("scheck:Tile has structure val=player", 0, player);

				pS = getTileStructure(map_coord(x), map_coord(y));

				// If correct type && player then complete & modify
				if (pS
				 && pS->pStructureType->type == type
				 && pS->player == player)
				{
					pS->direction = direction;
					pS->id = ref;

					if (pS->status != SS_BUILT)
					{
						pS->status = SS_BUILT;
						buildingComplete(pS);
					}

					NETlogEntry("scheck: fixed?", 0, player);
				}
				// Wall becoming a cornerwall
				else if (pS->pStructureType->type == REF_WALL)
				{
					if (psStats->type == REF_WALLCORNER)
					{
						NETlogEntry("scheck: fixed wall->cornerwall", 0, 0);
						removeStruct(pS, true);

						powerCalc(false);
						pS = buildStructure((STRUCTURE_STATS * )psStats, x, y, player, true);
						powerCalc(true);

						if (pS)
						{
							pS->id = ref;
						}
						else
						{
							NETlogEntry("scheck: failed to upgrade wall!", 0, player);
							return false;
						}
					}
				}
				else
				{
					NETlogEntry("scheck:Tile did not have correct type or player val=player",0,player);
					return false;
			    }
			}
			// Nothing exists there so lets get building!
			else
			{
				NETlogEntry("scheck: didn't find structure at all, building it",0,0);

				powerCalc(false);
				pS = buildStructure((STRUCTURE_STATS *) psStats, x, y, player, true);
				powerCalc(true);
			}
		}

		if (pS)
		{
			// Check its finished
			if (pS->status != SS_BUILT)
			{
				pS->direction = direction;
				pS->id = ref;
				pS->status = SS_BUILT;
				buildingComplete(pS);
			}

			// If the structure has a capacity
			switch (pS->pStructureType->type)
			{
				case REF_RESEARCH:
					ourCapacity = ((RESEARCH_FACILITY *) pS->pFunctionality)->capacity;
					j = researchModuleStat;
					break;
				case REF_FACTORY:
				case REF_VTOL_FACTORY:
					ourCapacity = ((FACTORY *) pS->pFunctionality)->capacity;
					j = factoryModuleStat;
					break;
				case REF_POWER_GEN:
					ourCapacity = ((POWER_GEN *) pS->pFunctionality)->capacity;
					j = powerModuleStat;
					break;
				default:
					hasCapacity = false;
					break;
			}

			// So long as the struct has a capacity fetch it from the packet
			if (hasCapacity)
			{
				uint8_t actualCapacity = 0;

				NETuint8_t(&actualCapacity);

				// If our capacity is different upgrade ourself
				for (; ourCapacity < actualCapacity; ourCapacity++)
				{
					buildStructure(&asStructureStats[j], pS->pos.x, pS->pos.y, pS->player, false);

					// Check it is finished
					if (pS && pS->status != SS_BUILT)
					{
						pS->id = ref;
						pS->status = SS_BUILT;
						buildingComplete(pS);
					}
				}
			}
		}

	NETend();
	return true;
}


// ////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////
// Power Checking. Send a power level check every now and again.
static BOOL sendPowerCheck()
{
	static UDWORD	lastsent = 0;
	uint8_t			player = selectedPlayer;
	uint32_t		power = getPower(player);

	if (lastsent > gameTime)
	{
		lastsent = 0;
	}

	// Only send if not done recently
	if (gameTime - lastsent < POWER_FREQUENCY)
	{
		return true;
	}

	lastsent = gameTime;

	NETbeginEncode(NETgameQueue(selectedPlayer), GAME_CHECK_POWER);
		NETuint8_t(&player);
		NETuint32_t(&power);
	return NETend();
}

BOOL recvPowerCheck(NETQUEUE queue)
{
	uint8_t		player;
	uint32_t	power, power2;

	NETbeginDecode(queue, GAME_CHECK_POWER);
		NETuint8_t(&player);
		NETuint32_t(&power);
	NETend();

	if (player >= MAX_PLAYERS)
	{
		debug(LOG_ERROR, "Bad GAME_CHECK_POWER packet: player is %d : %s",
		      (int)player, isHumanPlayer(player) ? "Human" : "AI");
		return false;
	}

	power2 = getPower(player);
	if (power != power2)
	{
//		debug(LOG_SYNC, "GAME_CHECK_POWER: Adjusting power for player %d (%s) from %u to %u",
//		      (int)player, isHumanPlayer(player) ? "Human" : "AI", power2, power);
		setPower( (uint32_t)player, power);
	}
	return true;
}

// ////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////
// Score
// We use setMultiStats() to broadcast the score when needed.
BOOL sendScoreCheck(void)
{
	static UDWORD	lastsent = 0;

	if (lastsent > gameTime)
	{
		lastsent= 0;
	}

	if (gameTime - lastsent < SCORE_FREQUENCY)
	{
		return true;
	}

	lastsent = gameTime;

	// Broadcast any changes in other players, but not in FRONTEND!!!
	if (titleMode != MULTIOPTION && titleMode != MULTILIMIT)
	{
		uint8_t			i;

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			PLAYERSTATS		stats;

			// Host controls AI's scores + his own...
			if (myResponsibility(i))
			{
				// Update local score
				stats = getMultiStats(i, true);

				// Add recently scored points
				stats.recentKills += stats.killsToAdd;
				stats.totalKills  += stats.killsToAdd;
				stats.recentScore += stats.scoreToAdd;
				stats.totalScore  += stats.scoreToAdd;

				// Zero them out
				stats.killsToAdd = stats.scoreToAdd = 0;

				// Send score to everyone else
				setMultiStats(i, stats, false);
			}
		}
	}

	return true;
}

// ////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////
// Pings

static UDWORD averagePing(void)
{
	UDWORD i, count = 0, total = 0;

	for(i=0;i<MAX_PLAYERS;i++)
	{
		if(isHumanPlayer(i))
		{
			total += ingame.PingTimes[i];
			count ++;
		}
	}
	return total / MAX(count, 1);
}

BOOL sendPing(void)
{
	BOOL			isNew = true;
	uint8_t			player = selectedPlayer;
	int				i;
	static UDWORD	lastPing = 0;	// Last time we sent a ping
	static UDWORD	lastav = 0;		// Last time we updated average

	// Only ping every so often
	if (lastPing > gameTime)
	{
		lastPing = 0;
	}

	if (gameTime - lastPing < PING_FREQUENCY)
	{
		return true;
	}

	lastPing = gameTime;

	// If host, also update the average ping stat for joiners
	if (NetPlay.isHost)
	{
		if (lastav > gameTime)
		{
			lastav = 0;
		}

		if (gameTime - lastav > AV_PING_FREQUENCY)
		{
			NETsetGameFlags(2, averagePing());
			lastav = gameTime;
		}
	}

	/*
	 * Before we send the ping, if any player failed to respond to the last one
	 * we should re-enumerate the players.
	 */

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		if (isHumanPlayer(i)
		 && PingSend[i]
		 && ingame.PingTimes[i]
		 && i != selectedPlayer)
		{
			ingame.PingTimes[i] = PING_LIMIT;
		}
		else if (!isHumanPlayer(i)
		      && PingSend[i]
		      && ingame.PingTimes[i]
		      && i != selectedPlayer)
		{
			ingame.PingTimes[i] = 0;
		}
	}

	NETbeginEncode(NETbroadcastQueue(), NET_PING);
		NETuint8_t(&player);
		NETbool(&isNew);
	NETend();

	// Note when we sent the ping
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		PingSend[i] = gameTime2;
	}

	return true;
}

// accept and process incoming ping messages.
BOOL recvPing(NETQUEUE queue)
{
	BOOL	isNew;
	uint8_t	sender, us = selectedPlayer;

	NETbeginDecode(queue, NET_PING);
		NETuint8_t(&sender);
		NETbool(&isNew);
	NETend();

	if (sender >= MAX_PLAYERS)
	{
		debug(LOG_ERROR, "Bad NET_PING packet, sender is %d", (int)sender);
		return false;
	}

	// If this is a new ping, respond to it
	if (isNew)
	{
		NETbeginEncode(NETnetQueue(sender), NET_PING);
			// We are responding to a new ping
			isNew = false;

			NETuint8_t(&us);
			NETbool(&isNew);
		NETend();
	}
	// They are responding to one of our pings
	else
	{
		// Work out how long it took them to respond
		ingame.PingTimes[sender] = (gameTime2 - PingSend[sender]) / 2;

		// Note that we have received it
		PingSend[sender] = 0;
	}

	return true;
}
