/* 
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/**
 * @file varz.h
 * Shared game-state globals: the bulk of the in-flight/gameplay state (enemies,
 * shots, explosions, level/event playback, HUD/menu scratch state, demo
 * recording, and misc timers) carried over from the original Pascal source.
 * Also declares the enemy/event/megamap record types these globals are made of.
 * Definitions live in varz.c; this header only lists `extern` declarations.
 */
#ifndef VARZ_H
#define VARZ_H

#include "episodes.h"
#include "opentyr.h"
#include "player.h"
#include "sprite.h"

#include <stdbool.h>

#define SA 7

enum
{
	SA_NONE = 0,
	SA_NORTSHIPZ = 7,
	
	// only used for code entry
	SA_DESTRUCT = 8,
	SA_ENGAGE = 9,
	
	// only used in pItems[P_SUPERARCADE]
	SA_SUPERTYRIAN = 254,
	SA_ARCADE = 255
};

#define ENEMY_SHOT_MAX  60 /* 60*/

#define CURRENT_KEY_SPEED 1  /*Keyboard/Joystick movement rate*/

#define MAX_EXPLOSIONS           200
#define MAX_REPEATING_EXPLOSIONS 20
#define MAX_SUPERPIXELS          101

struct JE_SingleEnemyType
{
	JE_byte     fillbyte;
	JE_integer  ex, ey;     /* POSITION */
	JE_shortint exc, eyc;   /* CURRENT SPEED */
	JE_shortint exca, eyca; /* RANDOM ACCELERATION */
	JE_shortint excc, eycc; /* FIXED ACCELERATION WAITTIME */
	JE_shortint exccw, eyccw;
	JE_byte     armorleft;
	JE_byte     eshotwait[3], eshotmultipos[3]; /* [1..3] */
	JE_byte     enemycycle;
	JE_byte     ani;
	JE_word     egr[20]; /* [1..20] */
	JE_byte     size;
	JE_byte     linknum;
	JE_byte     aniactive;
	JE_byte     animax;
	JE_byte     aniwhenfire;
	Sprite2_array *sprite2s;
	JE_shortint exrev, eyrev;
	JE_integer  exccadd, eyccadd;
	JE_byte     exccwmax, eyccwmax;
	void       *enemydatofs;
	JE_boolean  edamaged;
	JE_word     enemytype;
	JE_byte     animin;
	JE_word     edgr;
	JE_shortint edlevel;
	JE_shortint edani;
	JE_byte     fill1;
	JE_byte     filter;
	JE_integer  evalue;
	JE_integer  fixedmovey;
	JE_byte     freq[3]; /* [1..3] */
	JE_byte     launchwait;
	JE_word     launchtype;
	JE_byte     launchfreq;
	JE_byte     xaccel;
	JE_byte     yaccel;
	JE_byte     tur[3]; /* [1..3] */
	JE_word     enemydie; /* Enemy created when this one dies */
	JE_boolean  enemyground;
	JE_byte     explonum;
	JE_word     mapoffset;
	JE_boolean  scoreitem;

	JE_boolean  special;
	JE_byte     flagnum;
	JE_boolean  setto;

	JE_byte     iced; /*Duration*/

	JE_byte     launchspecial;

	JE_integer  xminbounce;
	JE_integer  xmaxbounce;
	JE_integer  yminbounce;
	JE_integer  ymaxbounce;
	JE_byte     fill[3]; /* [1..3] */
};

typedef struct JE_SingleEnemyType JE_MultiEnemyType[100]; /* [1..100] */

typedef JE_byte JE_DanCShape[24 * 28]; /* [1..(24*28) div 2] OF WORD */

typedef JE_char JE_CharString[256]; /* [1..256] */

typedef JE_byte JE_Map1Buffer[24 * 28 * 13 * 4]; /* [1..24*28*13*4] */

typedef JE_byte *JE_MapType[300][14]; /* [1..300, 1..14] */
typedef JE_byte *JE_MapType2[600][14]; /* [1..600, 1..14] */
typedef JE_byte *JE_MapType3[600][15]; /* [1..600, 1..15] */

struct JE_EventRecType
{
	JE_word     eventtime;
	JE_byte     eventtype;
	JE_integer  eventdat, eventdat2;
	JE_shortint eventdat3, eventdat5, eventdat6;
	JE_byte     eventdat4;
};

struct JE_MegaDataType1
{
	JE_MapType mainmap;
	struct
	{
		JE_DanCShape sh;
	} shapes[72]; /* [0..71] */
	JE_byte tempdat1;
	/*JE_DanCShape filler;*/
};

struct JE_MegaDataType2
{
	JE_MapType2 mainmap;
	struct
	{
		JE_byte nothing[3]; /* [1..3] */
		JE_byte fill;
		JE_DanCShape sh;
	} shapes[71]; /* [0..70] */
	JE_byte tempdat2;
};

struct JE_MegaDataType3
{
	JE_MapType3 mainmap;
	struct
	{
		JE_byte nothing[3]; /* [1..3] */
		JE_byte fill;
		JE_DanCShape sh;
	} shapes[70]; /* [0..69] */
	JE_byte tempdat3;
};

typedef JE_byte JE_EnemyAvailType[100]; /* [1..100] */

typedef struct {
	JE_integer sx, sy;
	JE_integer sxm, sym;
	JE_shortint sxc, syc;
	JE_byte tx, ty;
	JE_word sgr;
	JE_byte sdmg;
	JE_byte duration;
	JE_word animate;
	JE_word animax;
	JE_byte fill[12];
} EnemyShotType;

typedef struct {
	JE_byte ttl;
	JE_integer x, y;
	JE_word sprite;
	bool followPlayer;
	bool fixedPosition;
	JE_integer deltaY;
} Explosion;

typedef struct {
	unsigned int delay;
	unsigned int ttl;
	unsigned int x, y;
	bool big;
} rep_explosion_type;

typedef struct {
	unsigned int x, y, z;
	signed int delta_x, delta_y;
	Uint8 color;
} superpixel_type;

/* ===== Level/event scratch (used while decoding the current event record) ===== */
extern JE_integer tempDat, tempDat2, tempDat3; // scratch copies of the current eventRec's eventdat fields (new-enemy spawn params)

/* ===== SuperArcade / ship & weapon lookup tables (const, data-driven) ===== */
extern const JE_byte SANextShip[SA + 2];       // SuperArcade mode: next ship id per SA slot
extern const JE_word SASpecialWeapon[SA];      // SuperArcade mode: special weapon id per SA slot
extern const JE_word SASpecialWeaponB[SA];     // SuperArcade mode: alternate/second special weapon id per SA slot
extern const JE_byte SAShip[SA];               // SuperArcade mode: ship id per SA slot
extern const JE_word SAWeapon[SA][5];          // SuperArcade mode: front weapon ids per SA slot
extern const JE_byte specialArcadeWeapon[PORT_NUM]; // Arcade-mode special weapon id per weapon port
extern const JE_byte optionSelect[16][3][2];   // sidekick/option selection table (menu -> option type/index)
extern const JE_word PGR[21];                  // planet graphic (sprite) ids for the map screen
extern const JE_byte PAni[21];                 // planet animation frame counts for the map screen
extern const JE_word linkGunWeapons[38];       // weapon ids eligible for "link gun" combos
extern const JE_word chargeGunWeapons[38];     // weapon ids eligible for the charge-gun mechanic
extern const JE_byte randomEnemyLaunchSounds[3]; // sound ids used for random enemy weapon-launch sfx
extern const JE_byte keyboardCombos[26][8];    // Konami-style secret code entry: valid key combo table
extern const JE_byte shipCombosB[21];          // secret code entry: alternate ship-unlock combo table
extern const JE_byte superTyrianSpecials[4];   // Super Tyrian mode: special weapon ids
extern const JE_byte shipCombos[14][3];        // secret code entry: ship-unlock combo table

/* ===== Secret code entry state ===== */
extern JE_byte SFCurrentCode[2][21];           // per-player in-progress secret code keystrokes
extern JE_byte SFExecuted[2];                  // per-player: which secret code (if any) was just executed

/* ===== Level/event playback state ===== */
extern JE_byte lvlFileNum;                     // index of the currently loaded levelX.dat file
extern JE_word maxEvent, eventLoc;             // total event count and current playback index into eventRec
extern JE_word tempBackMove, explodeMove;      // background scroll speed snapshot; per-frame explosion drift
extern JE_byte levelEnd;                       // countdown (frames) driving the level-end sequence
extern JE_word levelEndFxWait;                 // delay timer for level-end visual/sound effects
extern JE_shortint levelEndWarp;               // vertical warp offset applied to ships during level-end
extern JE_boolean endLevel, reallyEndLevel, waitToEndLevel, playerEndLevel, normalBonusLevelCurrent, bonusLevelCurrent, smallEnemyAdjust, readyToEndLevel, quitRequested; // level-end/bonus-level/quit flags
extern JE_byte newPL[10];                      // scratch array for event-driven "set flag" (eventdat3/4) processing
extern JE_word returnLoc;                      // level index to resume at after a sub-level detour (e.g. Galaga mode)
extern JE_boolean returnActive;                // true while waiting to jump back to returnLoc
extern JE_word galagaShotFreq;                 // Galaga bonus mode: enemy shot frequency (ramps up over time)
extern JE_longint galagaLife;                  // Galaga bonus mode: cash threshold for the next extra life

/* ===== Debug / frame-timing instrumentation ===== */
extern JE_boolean debug;                       // global debug-mode flag
extern Uint32 debugTime, lastDebugTime;        // SDL tick snapshots used to measure frame time
extern Uint32 debugHistCount;                  // number of samples accumulated into debugHist
extern Uint32 debugHist;                       // accumulated frame-time history (debug overlay)

/* ===== Core flight/level progress state ===== */
extern JE_word curLoc;                         // current position (time) within the level's event timeline
extern JE_boolean firstGameOver, gameLoaded, enemyStillExploding; // game-over-once flag; save-loaded flag; enemy death fx still playing
extern JE_word totalEnemy;                     // total enemies spawned this level (for kill-percentage stat)
extern JE_word enemyKilled;                    // enemies killed this level
extern struct JE_MegaDataType1 megaData1;      // loaded tileset/map data, mainmap kind 1 (levels1.dat)
extern struct JE_MegaDataType2 megaData2;      // loaded tileset/map data, mainmap kind 2 (levels2.dat)
extern struct JE_MegaDataType3 megaData3;      // loaded tileset/map data, mainmap kind 3 (levels3.dat)
extern JE_byte flash;                          // current background "flash" color-cycle value
extern JE_shortint flashChange;                // per-frame delta applied to flash
extern JE_byte displayTime;                    // countdown for transient on-screen message display

/* ===== Demo recording/playback ===== */
extern bool play_demo, record_demo, stopped_demo; // demo playback/record mode flags; demo ended early
extern Uint8 demo_num;                         // index of the attract-mode demo file currently in use
extern bool editorPlaytest;                    // level-editor "F5" playtest of a staged level (see varz.c)
extern FILE *demo_file;                        // open handle to the demo recording/playback file

extern Uint8 demo_keys;                        // last input-key byte read from/written to the demo stream
extern Uint16 demo_keys_wait;                  // frames remaining before the next demo_keys sample

/* ===== Flight / in-game state ===== */
extern JE_byte soundQueue[8];                  // pending sound-effect ids queued for this frame (incl. Destruct)
extern JE_boolean enemyContinualDamage;        // event flag: enemy deals damage every frame on contact, not just once
extern JE_boolean enemiesActive;               // master switch for enemy spawning this level
extern JE_boolean forceEvents;                 // force event timeline to advance even when background is stopped
extern JE_boolean stopBackgrounds;             // freeze background scrolling
extern JE_byte stopBackgroundNum;              // which background layer(s) are stopped (bitmask/id)
extern JE_byte damageRate;                     // collision damage rate divisor/cap for the player ship
extern JE_boolean background3x1;               // background layer 3 uses the 3x1 (vs normal) tiling mode
extern JE_boolean background3x1b;              // secondary flag for the background layer-3 3x1 tiling mode
extern JE_boolean levelTimer;                  // whether the level has an active countdown timer
extern JE_word levelTimerCountdown;            // frames remaining on the level countdown timer
extern JE_word levelTimerJumpTo;               // event index to jump to when the level timer expires
extern JE_boolean randomExplosions;            // event flag: spawn random background explosions
extern JE_boolean editShip1, editShip2;        // per-player: ship-editor (secret code) unlocked this session
extern JE_boolean globalFlags[10];             // level-scripting global flags set/read by events (eventdat "set flag")
extern JE_byte levelSong;                      // music track id for the current level
extern JE_boolean loadDestruct;                // request to launch the Destruct minigame instead of normal flight
extern JE_word mapOrigin, mapPNum;             // map screen: origin planet id; number of planets in the current path
extern JE_byte mapPlanet[5], mapSection[5];    // map screen: planet id and level-section id per map path node
extern JE_boolean moveTyrianLogoUp;            // title screen: animate the Tyrian logo sliding upward
extern JE_boolean skipStarShowVGA;             // skip the starfield palette/VGA update for one frame
extern JE_MultiEnemyType enemy;                // live enemy slot table (position, ai, sprite, hp, ...)
extern JE_EnemyAvailType enemyAvail;           // per-enemy-slot availability/free-list flags
extern JE_word enemyOffset;                    // rolling index into `enemy` for the next enemy slot to spawn/scan
extern JE_word enemyOnScreen;                  // count of enemies currently active on screen
extern JE_word superEnemy254Jump;              // event index to jump to when the special "PL 254" enemy dies
extern Explosion explosions[MAX_EXPLOSIONS];   // active explosion sprite/animation slots
extern JE_integer explosionFollowAmountX, explosionFollowAmountY; // per-frame player-relative offset applied to player-following explosions
extern JE_boolean fireButtonHeld;              // tracks whether the fire button is being held (charge weapon input)
extern JE_boolean enemyShotAvail[ENEMY_SHOT_MAX]; // per-slot availability flags for enemyShot
extern EnemyShotType enemyShot[ENEMY_SHOT_MAX]; // active enemy projectile slots
extern JE_byte zinglonDuration;                // Zinglon boss-specific effect duration counter
extern JE_byte astralDuration;                 // Astral special-weapon effect duration counter
extern JE_word flareDuration;                  // screen-flare effect duration counter
extern JE_boolean flareStart;                  // trigger to begin a screen-flare effect
extern JE_shortint flareColChg;                // per-frame palette-flare color delta
extern JE_byte specialWait;                    // frames until the player's special weapon can fire again
extern JE_byte nextSpecialWait;                // pending value to assign to specialWait
extern JE_boolean spraySpecial;                // special-weapon "spray" mode flag
extern JE_byte doIced;                         // pending "freeze" duration to apply to the next hit enemy
extern JE_boolean infiniteShot;                // debug/cheat: unlimited special-weapon ammo
extern JE_boolean allPlayersGone;              // true once every player ship has been destroyed
extern const uint shadowYDist;                 // vertical offset used when drawing the player ship's ground shadow
extern JE_real optionSatelliteRotate;          // rotation angle (radians) for satellite-orbit sidekick options
extern JE_integer optionAttachmentMove;        // vertical animation offset for attachment-style sidekick options
extern JE_boolean optionAttachmentLinked, optionAttachmentReturn; // attachment sidekick: currently docked; returning-to-dock state
extern JE_byte chargeWait, chargeLevel, chargeMax, chargeGr, chargeGrWait; // charge-weapon: cooldown, current/max charge level, HUD frame, frame-advance timer
extern JE_word neat;                           // accumulating "detail" seed passed to JE_darkenBackground's dither
extern rep_explosion_type rep_explosions[MAX_REPEATING_EXPLOSIONS]; // looping/repeating background explosion slots
extern superpixel_type superpixels[MAX_SUPERPIXELS]; // "superpixel" particle-effect slots (JE_doSP)
extern unsigned int last_superpixel;           // index of the most recently spawned superpixel

/* ===== Misc scratch variables (shared temporaries used across gameplay/menu code) ===== */
extern JE_byte temp, temp2, temp3;             // general-purpose byte scratch variables
extern JE_word tempW;                          // general-purpose word scratch variable
extern JE_boolean doNotSaveBackup;             // suppress writing a config/save backup on next save
extern JE_word x, y;                           // general-purpose coordinate scratch variables
extern JE_integer b;                           // general-purpose scratch variable (loop index / shot handle)
extern JE_byte **BKwrap1to, **BKwrap2to, **BKwrap3to, **BKwrap1, **BKwrap2, **BKwrap3; // background map-row wrap pointers/targets for the 3 parallax layers
extern JE_shortint specialWeaponFilter, specialWeaponFreq; // unused/legacy: no remaining references outside varz.c
extern JE_word specialWeaponWpn;               // unused/legacy: no remaining references outside varz.c
extern JE_boolean linkToPlayer;                // unused/legacy: no remaining references outside varz.c
extern JE_word shipGr, shipGr2;                // current ship sprite graphic index, per player
extern Sprite2_array *shipGrPtr, *shipGr2ptr;  // sprite sheet backing shipGr/shipGr2

static const int hud_sidekick_y[2][2] =
{
	{  64,  82 }, // one player HUD
	{ 108, 126 }, // two player HUD
};

void JE_getShipInfo(void);
JE_word JE_SGr(JE_word ship, Sprite2_array **ptr);

void JE_drawOptions(void);

void JE_tyrianHalt(JE_byte code); /* This ends the game */
void JE_specialComplete(JE_byte playernum, JE_byte specialType);
void JE_doSpecialShot(JE_byte playernum, uint *armor, uint *shield);

void JE_wipeShieldArmorBars(void);
JE_byte JE_playerDamage(JE_byte temp, Player *);

void JE_setupExplosion(JE_integer x, JE_integer y, JE_integer deltaY, JE_integer type, bool fixedPosition, bool followPlayer);
void JE_setupExplosionLarge(JE_boolean enemyground, JE_byte explonum, JE_integer x, JE_integer y);

void JE_drawShield(void);
void JE_drawArmor(void);

JE_word JE_portConfigs(void);

/*SuperPixels*/
void JE_doSP(JE_word x, JE_word y, JE_word num, JE_byte explowidth, JE_byte color);
void JE_drawSP(void);

void JE_drawOptionLevel(void);

#endif /* VARZ_H */
