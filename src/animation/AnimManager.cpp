#include "common.h"

#include "General.h"
#include "RwHelper.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "FileMgr.h"
#include "RpAnimBlend.h"
#include "AnimBlendClumpData.h"
#include "AnimBlendAssociation.h"
#include "AnimBlendAssocGroup.h"
#include "AnimManager.h"
#include "Streaming.h"
#include "main.h"

#include <new>

CAnimBlock CAnimManager::ms_aAnimBlocks[NUMANIMBLOCKS];
CAnimBlendHierarchy CAnimManager::ms_aAnimations[NUMANIMATIONS];
int32 CAnimManager::ms_numAnimBlocks;
int32 CAnimManager::ms_numAnimations;
CAnimBlendAssocGroup *CAnimManager::ms_aAnimAssocGroups;
CLinkList<CAnimBlendHierarchy*> CAnimManager::ms_animCache;

AnimAssocDesc aStdAnimDescs[] = {
	{ ANIM_STD_WALK, ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_HAS_TRANSLATION | ASSOC_WALK },
	{ ANIM_STD_RUN, ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_HAS_TRANSLATION | ASSOC_WALK },
	{ ANIM_STD_RUNFAST, ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_HAS_TRANSLATION | ASSOC_WALK },
	{ ANIM_STD_IDLE, ASSOC_REPEAT },
	{ ANIM_STD_STARTWALK, ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_RUNSTOP1, ASSOC_DELETEFADEDOUT | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_RUNSTOP2, ASSOC_DELETEFADEDOUT | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_IDLE_CAM, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_STD_IDLE_HBHB, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_STD_IDLE_TIRED, ASSOC_REPEAT },
	{ ANIM_STD_IDLE_BIGGUN, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_STD_CHAT, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_STD_HAILTAXI, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_KO_FRONT, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_FRONTAL },
	{ ANIM_STD_KO_LEFT, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_FRONTAL },
	{ ANIM_STD_KO_BACK, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_FRONTAL },
	{ ANIM_STD_KO_RIGHT, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_FRONTAL },
	{ ANIM_STD_KO_SHOT_FACE, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_FRONTAL },
	{ ANIM_STD_KO_SHOT_STOMACH, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_KO_SHOT_ARM_L, ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_KO_SHOT_ARM_R, ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_KO_SHOT_LEG_L, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_KO_SHOT_LEG_R, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_SPINFORWARD_LEFT, ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_SPINFORWARD_RIGHT, ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_HIGHIMPACT_FRONT, ASSOC_PARTIAL },
	{ ANIM_STD_HIGHIMPACT_LEFT, ASSOC_PARTIAL },
	{ ANIM_STD_HIGHIMPACT_BACK, ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_HIGHIMPACT_RIGHT, ASSOC_PARTIAL },
	{ ANIM_STD_HITBYGUN_FRONT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_STD_HITBYGUN_LEFT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_STD_HITBYGUN_BACK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_STD_HITBYGUN_RIGHT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_STD_HIT_FRONT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_LEFT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_HIT_BACK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_RIGHT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_HIT_FLOOR, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_HIT_BODYBLOW, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_CHEST, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_HEAD, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_WALK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_WALL, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HIT_FLOOR_FRONT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_HIT_BEHIND, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_IDLE, ASSOC_REPEAT },
	{ ANIM_STD_FIGHT_2IDLE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_SHUFFLE_F, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_FIGHT_BODYBLOW, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_HEAD, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_KICK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_KNEE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_LHOOK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_PUNCH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_ROUNDHOUSE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_FIGHT_LONGKICK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_PARTIAL_PUNCH, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_STD_FIGHT_JAB, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_ELBOW_L, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_ELBOW_R, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_BKICK_L, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_BKICK_R, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_DETONATE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_PUNCH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_PARTIALPUNCH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_KICKGROUND, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_THROW_UNDER, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_FIGHT_SHUFFLE_B, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_JACKEDCAR_RHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_JACKEDCAR_LO_RHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_JACKEDCAR_LHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_JACKEDCAR_LO_LHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_QUICKJACK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_QUICKJACKED, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_ALIGN_DOOR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_ALIGNHI_DOOR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_OPEN_DOOR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CARDOOR_LOCKED_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_PULL_OUT_PED_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_PULL_OUT_PED_LO_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_GET_IN_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_GET_IN_LO_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_DOOR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_DOOR_LO_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_DOOR_ROLLING_LO_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_JUMP_IN_LO_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_GETOUT_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_GETOUT_LO_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_ALIGN_DOOR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_ALIGNHI_DOOR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_OPEN_DOOR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CARDOOR_LOCKED_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_PULL_OUT_PED_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_PULL_OUT_PED_LO_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_GET_IN_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_GET_IN_LO_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_DOOR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_DOOR_LO_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_SHUFFLE_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_SHUFFLE_LO_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_SIT, ASSOC_DELETEFADEDOUT},
	{ ANIM_STD_CAR_SIT_LO, ASSOC_DELETEFADEDOUT},
	{ ANIM_STD_CAR_SIT_P, ASSOC_DELETEFADEDOUT},
	{ ANIM_STD_CAR_SIT_P_LO, ASSOC_DELETEFADEDOUT},
	{ ANIM_STD_CAR_DRIVE_LEFT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVE_RIGHT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVE_LEFT_LO, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVE_RIGHT_LO, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVEBY_LEFT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVEBY_RIGHT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVEBY_LEFT_LO, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_DRIVEBY_RIGHT_LO, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_CAR_LOOKBEHIND, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_BOAT_DRIVE, ASSOC_DELETEFADEDOUT | ASSOC_DRIVING },
	{ ANIM_STD_BOAT_DRIVE_LEFT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_BOAT_DRIVE_RIGHT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_BOAT_LOOKBEHIND, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_STD_BIKE_PICKUP_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_BIKE_PICKUP_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_BIKE_PULLUP_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_BIKE_PULLUP_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_BIKE_ELBOW_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_BIKE_ELBOW_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_BIKE_FALLOFF, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_BIKE_FALLBACK, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_GETOUT_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_GETOUT_LO_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_CLOSE_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CAR_HOOKERTALK, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_STD_TRAIN_GETIN, ASSOC_PARTIAL },
	{ ANIM_STD_TRAIN_GETOUT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CRAWLOUT_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_CRAWLOUT_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_ROLLOUT_LHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION },
	{ ANIM_STD_ROLLOUT_RHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION },
	{ ANIM_STD_GET_UP, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_GET_UP_LEFT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_GET_UP_RIGHT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_GET_UP_FRONT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_JUMP_LAUNCH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_JUMP_GLIDE, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_JUMP_LAND, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_FALL, ASSOC_REPEAT | ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_FALL_GLIDE, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_FALL_LAND, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_FALL_COLLAPSE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_FALL_ONBACK, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_FALL_ONFRONT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_FRONTAL },
	{ ANIM_STD_EVADE_STEP, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_EVADE_DIVE, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_FRONTAL },
	{ ANIM_STD_XPRESS_SCRATCH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_IDLE },
	{ ANIM_STD_ROADCROSS, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_STD_TURN180, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_ARREST, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_DROWN, ASSOC_PARTIAL },
	{ ANIM_STD_DUCK_DOWN, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_DUCK_LOW, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_DUCK_WEAPON, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_RBLOCK_SHOOT, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_HANDSUP, ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_HANDSCOWER, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_STD_PARTIAL_FUCKU, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_STD_PHONE_IN, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_PHONE_OUT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_PHONE_TALK, ASSOC_REPEAT | ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
	{ ANIM_STD_SEAT_DOWN, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_SEAT_UP, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_SEAT_IDLE, ASSOC_REPEAT | ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_SEAT_RVRS, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_ATM, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_ABSEIL, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL },
};
AnimAssocDesc aVanAnimDescs[] = {
	{ ANIM_STD_VAN_OPEN_DOOR_REAR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_GET_IN_REAR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_CLOSE_DOOR_REAR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_GET_OUT_REAR_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_OPEN_DOOR_REAR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_GET_IN_REAR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_CLOSE_DOOR_REAR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_VAN_GET_OUT_REAR_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aCoachAnimDescs[] = {
	{ ANIM_STD_COACH_OPEN_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_COACH_OPEN_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_COACH_GET_IN_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_COACH_GET_IN_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STD_COACH_GET_OUT_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aBikeAnimDescs[] = {
	{ ANIM_BIKE_RIDE, ASSOC_DELETEFADEDOUT},
	{ ANIM_BIKE_READY, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_LEFT, ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_RIGHT, ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_LEANB, ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_LEANF, ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_WALKBACK, ASSOC_REPEAT | ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_JUMPON_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_BIKE_JUMPON_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_BIKE_KICK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_BIKE_HIT, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_BIKE_GETOFF_LHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_BIKE_GETOFF_RHS, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_BIKE_GETOFF_BACK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
	{ ANIM_BIKE_DRIVEBY_LHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_DRIVEBY_RHS, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_DRIVEBY_FORWARD, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_DRIVING },
	{ ANIM_BIKE_RIDE_P, ASSOC_DELETEFADEDOUT | ASSOC_DRIVING },
};
AnimAssocDesc aMeleeAnimDescs[] = {
	{ ANIM_MELEE_ATTACK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_MELEE_ATTACK_2ND, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_MELEE_ATTACK_START, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_NOWALK },
	{ ANIM_MELEE_IDLE_FIGHTMODE, ASSOC_REPEAT },
	{ ANIM_MELEE_ATTACK_FINISH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION },
};
AnimAssocDesc aSwingAnimDescs[] = {
	{ ANIM_MELEE_ATTACK, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_MELEE_ATTACK_2ND, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_MELEE_ATTACK_START, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_MELEE_IDLE_FIGHTMODE, ASSOC_REPEAT },
	{ ANIM_MELEE_ATTACK_FINISH, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aWeaponAnimDescs[] = {
	{ ANIM_WEAPON_FIRE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_WEAPON_CROUCHFIRE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_WEAPON_RELOAD, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_WEAPON_CROUCHRELOAD, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_WEAPON_FIRE_3RD, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aMedicAnimDescs[] = {
	{ ANIM_MEDIC_CPR, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aSunbatheAnimDescs[] = {
	{ ANIM_SUNBATHE_IDLE, ASSOC_REPEAT | ASSOC_PARTIAL },
	{ ANIM_SUNBATHE_DOWN, ASSOC_REPEAT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION },
	{ ANIM_SUNBATHE_UP, ASSOC_REPEAT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION },
	{ ANIM_SUNBATHE_ESCAPE, ASSOC_REPEAT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION },
};
AnimAssocDesc aPlayerIdleAnimDescs[] = {
	{ ANIM_PLAYER_IDLE1, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_PLAYER_IDLE2, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_PLAYER_IDLE3, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_PLAYER_IDLE4, ASSOC_DELETEFADEDOUT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aRiotAnimDescs[] = {
	{ ANIM_RIOT_ANGRY, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_RIOT_ANGRY_B, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_RIOT_CHANT, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_RIOT_PUNCHES, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_RIOT_SHOUT, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_RIOT_CHALLENGE, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_RIOT_FUCKYOU, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
AnimAssocDesc aStripAnimDescs[] = {
	{ ANIM_STRIP_A, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STRIP_B, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STRIP_C, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STRIP_D, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STRIP_E, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STRIP_F, ASSOC_REPEAT | ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
	{ ANIM_STRIP_G, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL },
};
#ifdef PC_PLAYER_CONTROLS
AnimAssocDesc aStdAnimDescsSide[] = {
	{ ANIM_STD_WALK, ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION | ASSOC_WALK },
	{ ANIM_STD_RUN, ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION | ASSOC_WALK },
	{ ANIM_STD_RUNFAST, ASSOC_REPEAT | ASSOC_MOVEMENT | ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION | ASSOC_WALK },
	{ ANIM_STD_IDLE, ASSOC_REPEAT },
	{ ANIM_STD_STARTWALK, ASSOC_HAS_TRANSLATION | ASSOC_HAS_X_TRANSLATION },
};
#endif
char const* aStdAnimations[] = {
	"walk_civi",
	"run_civi",
	"sprint_panic",
	"idle_stance",
	"walk_start",
	"run_stop",
	"run_stopR",
	"idle_hbhb",
	"idle_hbhb",
	"idle_tired",
	"idle_armed",
	"idle_chat",
	"idle_taxi",
	"KO_shot_front",
	"KO_shot_front",
	"KO_shot_front",
	"KO_shot_front",
	"KO_shot_face",
	"KO_shot_stom",
	"KO_shot_arml",
	"KO_shot_armR",
	"KO_shot_legl",
	"KO_shot_legR",
	"KD_left",
	"KD_right",
	"KO_skid_front",
	"KO_spin_R",
	"KO_skid_back",
	"KO_spin_L",
	"SHOT_partial",
	"SHOT_leftP",
	"SHOT_partial",
	"SHOT_rightP",
	"HIT_front",
	"HIT_L",
	"HIT_back",
	"HIT_R",
	"FLOOR_hit",
	"HIT_bodyblow",
	"HIT_chest",
	"HIT_head",
	"HIT_walk",
	"HIT_wall",
	"FLOOR_hit_f",
	"HIT_behind",
	"FIGHTIDLE",
	"FIGHT2IDLE",
	"FIGHTsh_F",
	"FIGHTbodyblow",
	"FIGHThead",
	"FIGHTkick",
	"FIGHTknee",
	"FIGHTLhook",
	"FIGHTpunch",
	"FIGHTrndhse",
	"FIGHTlngkck",
	"FIGHTppunch",
	"FIGHTjab",
	"FIGHTelbowL",
	"FIGHTelbowR",
	"FIGHTbkickL",
	"FIGHTbkickR",
	"bomber",
	"punchR",
	"FIGHTppunch",
	"KICK_floor",
	"WEAPON_throwu",
	"FIGHTsh_back",
	"car_jackedRHS",
	"car_LjackedRHS",
	"car_jackedLHS",
	"car_LjackedLHS",
	"CAR_Qjack",
	"CAR_Qjacked",
	"CAR_align_LHS",
	"CAR_alignHI_LHS",
	"CAR_open_LHS",
	"CAR_doorlocked_LHS",
	"CAR_pullout_LHS",
	"CAR_pulloutL_LHS",
	"CAR_getin_LHS",
	"CAR_getinL_LHS",
	"CAR_closedoor_LHS",
	"CAR_closedoorL_LHS",
	"CAR_rolldoor",
	"CAR_rolldoorLO",
	"CAR_jumpin_LHS",
	"CAR_getout_LHS",
	"CAR_getoutL_LHS",
	"CAR_close_LHS",
	"CAR_align_RHS",
	"CAR_alignHI_RHS",
	"CAR_open_RHS",
	"CAR_doorlocked_RHS",
	"CAR_pullout_RHS",
	"CAR_pulloutL_RHS",
	"CAR_getin_RHS",
	"CAR_getinL_RHS",
	"CAR_closedoor_RHS",
	"CAR_closedoorL_RHS",
	"CAR_shuffle_RHS",
	"CAR_Lshuffle_RHS",
	"CAR_sit",
	"CAR_Lsit",
	"CAR_sitp",
	"CAR_sitpLO",
	"DRIVE_L",
	"Drive_R",
	"Drive_LO_l",
	"Drive_LO_R",
	"Driveby_L",
	"Driveby_R",
	"DrivebyL_L",
	"DrivebyL_R",
	"CAR_LB",
	"DRIVE_BOAT",
	"DRIVE_BOAT_L",
	"DRIVE_BOAT_R",
	"DRIVE_BOAT_back",
	"BIKE_pickupR",
	"BIKE_pickupL",
	"BIKE_pullupR",
	"BIKE_pullupL",
	"BIKE_elbowR",
	"BIKE_elbowL",
	"BIKE_fall_off",
	"BIKE_fallR",
	"CAR_getout_RHS",
	"CAR_getoutL_RHS",
	"CAR_close_RHS",
	"car_hookertalk",
	"idle_stance",
	"idle_stance",
	"CAR_crawloutRHS",
	"CAR_crawloutRHS",
	"CAR_rollout_LHS",
	"CAR_rollout_LHS",
	"Getup",
	"Getup",
	"Getup",
	"Getup_front",
	"JUMP_launch",
	"JUMP_glide",
	"JUMP_land",
	"FALL_fall",
	"FALL_glide",
	"FALL_land",
	"FALL_collapse",
	"FALL_back",
	"FALL_front",
	"EV_step",
	"EV_dive",
	"XPRESSscratch",
	"roadcross",
	"TURN_180",
	"ARRESTgun",
	"DROWN",
	"DUCK_down",
	"DUCK_low",
	"WEAPON_crouch",
	"RBLOCK_Cshoot",
	"handsup",
	"handsCOWER",
	"FUCKU",
	"PHONE_in",
	"PHONE_out",
	"PHONE_talk",
	"SEAT_down",
	"SEAT_up",
	"SEAT_idle",
	"SEAT_down",
	"ATM",
	"abseil",
};
char const* aVanAnimations[] = {
	"VAN_openL",
	"VAN_getinL",
	"VAN_closeL",
	"VAN_getoutL",
	"VAN_open",
	"VAN_getin",
	"VAN_close",
	"VAN_getout",
};
char const* aCoachAnimations[] = {
	"COACH_opnL",
	"COACH_opnL",
	"COACH_inL",
	"COACH_inL",
	"COACH_outL",
};
char const* aBikesAnimations[] = {
	"BIKEs_Ride",
	"BIKEs_Still",
	"BIKEs_Left",
	"BIKEs_Right",
	"BIKEs_Back",
	"BIKEs_Fwd",
	"BIKEs_pushes",
	"BIKEs_jumponR",
	"BIKEs_jumponL",
	"BIKEs_kick",
	"BIKEs_hit",
	"BIKEs_getoffRHS",
	"BIKEs_getoffLHS",
	"BIKEs_getoffBACK",
	"BIKEs_drivebyLHS",
	"BIKEs_drivebyRHS",
	"BIKEs_drivebyFT",
	"BIKEs_passenger",
};
char const* aBikevAnimations[] = {
	"BIKEv_Ride",
	"BIKEv_Still",
	"BIKEv_Left",
	"BIKEv_Right",
	"BIKEv_Back",
	"BIKEv_Fwd",
	"BIKEv_pushes",
	"BIKEv_jumponR",
	"BIKEv_jumponL",
	"BIKEv_kick",
	"BIKEv_hit",
	"BIKEv_getoffRHS",
	"BIKEv_getoffLHS",
	"BIKEv_getoffBACK",
	"BIKEv_drivebyLHS",
	"BIKEv_drivebyRHS",
	"BIKEv_drivebyFT",
	"BIKEv_passenger",
};
char const* aBikehAnimations[] = {
	"BIKEh_Ride",
	"BIKEh_Still",
	"BIKEh_Left",
	"BIKEh_Right",
	"BIKEh_Back",
	"BIKEh_Fwd",
	"BIKEh_pushes",
	"BIKEh_jumponR",
	"BIKEh_jumponL",
	"BIKEh_kick",
	"BIKEh_hit",
	"BIKEh_getoffRHS",
	"BIKEh_getoffLHS",
	"BIKEh_getoffBACK",
	"BIKEh_drivebyLHS",
	"BIKEh_drivebyRHS",
	"BIKEh_drivebyFT",
	"BIKEh_passenger",
};
char const* aBikedAnimations[] = {
	"BIKEd_Ride",
	"BIKEd_Still",
	"BIKEd_Left",
	"BIKEd_Right",
	"BIKEd_Back",
	"BIKEd_Fwd",
	"BIKEd_pushes",
	"BIKEd_jumponR",
	"BIKEd_jumponL",
	"BIKEd_kick",
	"BIKEd_hit",
	"BIKEd_getoffRHS",
	"BIKEd_getoffLHS",
	"BIKEd_getoffBACK",
	"BIKEd_drivebyLHS",
	"BIKEd_drivebyRHS",
	"BIKEd_drivebyFT",
	"BIKEd_passenger",
};
char const* aUnarmedAnimations[] = {
	"punchR",
	"KICK_floor",
	"FIGHTppunch",
};
char const* aScrewdriverAnimations[] = {
	"FIGHTbodyblow",
	"FIGHTbodyblow",
	"FIGHTppunch",
	"FIGHTIDLE",
	"FIGHTbodyblow",
};
char const* aKnifeAnimations[] = {
	"WEAPON_knife_1",
	"WEAPON_knife_2",
	"knife_part",
	"WEAPON_knifeidle",
	"WEAPON_knife_3",
};
char const* aBaseballbatAnimations[] = {
	"WEAPON_bat_h",
	"WEAPON_bat_v",
	"BAT_PART",
	"WEAPON_bat_h",
	"WEAPON_golfclub",
};
char const* aGolfclubAnimations[] = {
	"WEAPON_bat_h",
	"WEAPON_golfclub",
	"BAT_PART",
	"WEAPON_bat_h",
	"WEAPON_bat_v",
};
char const* aChainsawAnimations[] = {
	"WEAPON_csaw",
	"WEAPON_csawlo",
	"csaw_part",
};
char const* aPythonAnimations[] = {
	"python_fire",
	"python_crouchfire",
	"python_reload",
	"python_crouchreload",
};
char const* aColtAnimations[] = {
	"colt45_fire",
	"colt45_crouchfire",
	"colt45_reload",
	"colt45_crouchreload",
	"colt45_cop",
};
char const* aShotgunAnimations[] = {
	"shotgun_fire",
	"shotgun_crouchfire",
};
char const* aBuddyAnimations[] = {
	"buddy_fire",
	"buddy_crouchfire",
};
char const* aTecAnimations[] = {
	"TEC_fire",
	"TEC_crouchfire",
	"TEC_reload",
	"TEC_crouchreload",
};
char const* aUziAnimations[] = {
	"UZI_fire",
	"UZI_crouchfire",
	"UZI_reload",
	"UZI_crouchreload",
};
char const* aRifleAnimations[] = {
	"RIFLE_fire",
	"RIFLE_crouchfire",
	"RIFLE_load",
	"RIFLE_crouchload",
};
char const* aM60Animations[] = {
	"M60_fire",
	"M60_fire",
	"M60_reload",
};
char const* aSniperAnimations[] = {
	"WEAPON_sniper",
};
char const* aThrowAnimations[] = {
	"WEAPON_throw",
	"WEAPON_throwu",
	"WEAPON_start_throw",
};
char const* aFlamethrowerAnimations[] = {
	"FLAME_fire",
};
char const* aMedicAnimations[] = {
	"CPR",
};
char const* aSunbatheAnimations[] = {
	"bather",
	"batherdown",
	"batherup",
	"batherscape",
};
char const* aPlayerIdleAnimations[] = {
	"stretch",
	"time",
	"shldr",
	"strleg",
};
char const* aRiotAnimations[] = {
	"riot_angry",
	"riot_angry_b",
	"riot_chant",
	"riot_punches",
	"riot_shout",
	"riot_challenge",
	"riot_fuku",
};
char const* aStripAnimations[] = {
	"strip_A",
	"strip_B",
	"strip_C",
	"strip_D",
	"strip_E",
	"strip_F",
	"strip_G",
};
char const* aLanceAnimations[] = {
	"lance",
};
char const* aPlayerAnimations[] = {
	"walk_player",
	"run_player",
	"SPRINT_civi",
	"IDLE_STANCE",
	"walk_start",
};
char const* aPlayerWithRocketAnimations[] = {
	"walk_rocket",
	"run_rocket",
	"run_rocket",
	"idle_rocket",
	"walk_start_rocket",
};
char const* aPlayer1ArmedAnimations[] = {
	"walk_player",
	"run_1armed",
	"SPRINT_civi",
	"IDLE_STANCE",
	"walk_start",
};
char const* aPlayer2ArmedAnimations[] = {
	"walk_armed",
	"run_armed",
	"run_armed",
	"idle_armed",
	"walk_start_armed",
};
char const* aPlayerBBBatAnimations[] = {
	"walk_player",
	"run_player",
	"run_player",
	"IDLE_STANCE",
	"walk_start",
};
char const* aPlayerChainsawAnimations[] = {
	"walk_csaw",
	"run_csaw",
	"run_csaw",
	"IDLE_csaw",
	"walk_start_csaw",
};
char const* aShuffleAnimations[] = {
	"WALK_shuffle",
	"RUN_civi",
	"SPRINT_civi",
	"IDLE_STANCE",
};
char const* aOldAnimations[] = {
	"walk_old",
	"run_civi",
	"sprint_civi",
	"idle_stance",
};
char const* aGang1Animations[] = {
	"walk_gang1",
	"run_gang1",
	"sprint_civi",
	"idle_stance",
};
char const* aGang2Animations[] = {
	"walk_gang2",
	"run_gang1",
	"sprint_civi",
	"idle_stance",
};
char const* aFatAnimations[] = {
	"walk_fat",
	"run_civi",
	"woman_runpanic",
	"idle_stance",
};
char const* aOldFatAnimations[] = {
	"walk_fatold",
	"run_fatold",
	"woman_runpanic",
	"idle_stance",
};
char const* aJoggerAnimations[] = {
	"JOG_maleA",
	"run_civi",
	"sprint_civi",
	"idle_stance",
};
char const* aStdWomanAnimations[] = {
	"woman_walknorm",
	"woman_run",
	"woman_runpanic",
	"woman_idlestance",
};
char const* aWomanShopAnimations[] = {
	"woman_walkshop",
	"woman_run",
	"woman_run",
	"woman_idlestance",
};
char const* aBusyWomanAnimations[] = {
	"woman_walkbusy",
	"woman_run",
	"woman_runpanic",
	"woman_idlestance",
};
char const* aSexyWomanAnimations[] = {
	"woman_walksexy",
	"woman_run",
	"woman_runpanic",
	"woman_idlestance",
};
char const* aFatWomanAnimations[] = {
	"walk_fat",
	"woman_run",
	"woman_runpanic",
	"woman_idlestance",
};
char const* aOldWomanAnimations[] = {
	"woman_walkold",
	"woman_run",
	"woman_runpanic",
	"woman_idlestance",
};
char const* aJoggerWomanAnimations[] = {
	"JOG_maleB",
	"woman_run",
	"woman_runpanic",
	"woman_idlestance",
};
char const* aPanicChunkyAnimations[] = {
	"run_fatold",
	"woman_runpanic",
	"woman_runpanic",
	"idle_stance",
};
char const* aSkateAnimations[] = {
	"skate_run",
	"skate_sprint",
	"skate_sprint",
	"skate_idle",
};
#ifdef PC_PLAYER_CONTROLS
char const* aPlayerStrafeBackAnimations[] = {
	"walk_back",
	"run_back",
	"run_back",
	"IDLE_STANCE",
	"walk_start_back",
};
char const* aPlayerStrafeLeftAnimations[] = {
	"walk_left",
	"run_left",
	"run_left",
	"IDLE_STANCE",
	"walk_start_left",
};
char const* aPlayerStrafeRightAnimations[] = {
	"walk_right",
	"run_right",
	"run_right",
	"IDLE_STANCE",
	"walk_start_right",
};
char const* aRocketStrafeBackAnimations[] = {
	"walk_rocket_back",
	"run_rocket_back",
	"run_rocket_back",
	"idle_rocket",
	"walkst_rocket_back",
};
char const* aRocketStrafeLeftAnimations[] = {
	"walk_rocket_left",
	"run_rocket_left",
	"run_rocket_left",
	"idle_rocket",
	"walkst_rocket_left",
};
char const* aRocketStrafeRightAnimations[] = {
	"walk_rocket_right",
	"run_rocket_right",
	"run_rocket_right",
	"idle_rocket",
	"walkst_rocket_right",
};
char const* aChainsawStrafeBackAnimations[] = {
	"walk_csaw_back",
	"run_csaw_back",
	"run_csaw_back",
	"idle_csaw",
	"walkst_csaw_back",
};
char const* aChainsawStrafeLeftAnimations[] = {
	"walk_csaw_left",
	"run_csaw_left",
	"run_csaw_left",
	"idle_csaw",
	"walkst_csaw_left",
};
char const* aChainsawStrafeRightAnimations[] = {
	"walk_csaw_right",
	"run_csaw_right",
	"run_csaw_right",
	"idle_csaw",
	"walkst_csaw_right",
};
#endif

#define awc(a) ARRAY_SIZE(a), a
const AnimAssocDefinition CAnimManager::ms_aAnimAssocDefinitions[NUM_ANIM_ASSOC_GROUPS] = {
	{ "man", "ped", MI_COP, awc(aStdAnimations), aStdAnimDescs },
	{ "van", "van", MI_COP, awc(aVanAnimations), aVanAnimDescs },
	{ "coach", "coach", MI_COP, awc(aCoachAnimations), aCoachAnimDescs },
	{ "bikes", "bikes", MI_COP, awc(aBikesAnimations), aBikeAnimDescs },
	{ "bikev", "bikev", MI_COP, awc(aBikevAnimations), aBikeAnimDescs },
	{ "bikeh", "bikeh", MI_COP, awc(aBikehAnimations), aBikeAnimDescs },
	{ "biked", "biked", MI_COP, awc(aBikedAnimations), aBikeAnimDescs },
	{ "unarmed", "ped", MI_COP, awc(aUnarmedAnimations), aMeleeAnimDescs },
	{ "screwdrv", "ped", MI_COP, awc(aScrewdriverAnimations), aMeleeAnimDescs },
	{ "knife", "knife", MI_COP, awc(aKnifeAnimations), aMeleeAnimDescs },
	{ "baseball", "baseball", MI_COP, awc(aBaseballbatAnimations), aSwingAnimDescs },
	{ "golfclub", "baseball", MI_COP, awc(aGolfclubAnimations), aSwingAnimDescs },
	{ "chainsaw", "chainsaw", MI_COP, awc(aChainsawAnimations), aMeleeAnimDescs },
	{ "python", "python", MI_COP, awc(aPythonAnimations), aWeaponAnimDescs },
	{ "colt45", "colt45", MI_COP, awc(aColtAnimations), aWeaponAnimDescs },
	{ "shotgun", "shotgun", MI_COP, awc(aShotgunAnimations), aWeaponAnimDescs },
	{ "buddy", "buddy", MI_COP, awc(aBuddyAnimations), aWeaponAnimDescs },
	{ "tec", "tec", MI_COP, awc(aTecAnimations), aWeaponAnimDescs },
	{ "uzi", "uzi", MI_COP, awc(aUziAnimations), aWeaponAnimDescs },
	{ "rifle", "rifle", MI_COP, awc(aRifleAnimations), aWeaponAnimDescs },
	{ "m60", "m60", MI_COP, awc(aM60Animations), aWeaponAnimDescs },
	{ "sniper", "sniper", MI_COP, awc(aSniperAnimations), aWeaponAnimDescs },
	{ "grenade", "grenade", MI_COP, awc(aThrowAnimations), aWeaponAnimDescs },
	{ "flame", "flame", MI_COP, awc(aFlamethrowerAnimations), aWeaponAnimDescs },
	{ "medic", "medic", MI_COP, awc(aMedicAnimations), aMedicAnimDescs },
	{ "sunbathe", "sunbathe", MI_COP, 1, aSunbatheAnimations, aSunbatheAnimDescs },	// NB: not using awc here!
	{ "playidles", "playidles", MI_COP, awc(aPlayerIdleAnimations), aPlayerIdleAnimDescs },
	{ "riot", "riot", MI_COP, awc(aRiotAnimations), aRiotAnimDescs },
	{ "strip", "strip", MI_COP, awc(aStripAnimations), aStripAnimDescs },
	{ "lance", "lance", MI_COP, awc(aLanceAnimations), aSunbatheAnimDescs },
	{ "player", "ped", MI_COP, awc(aPlayerAnimations), aStdAnimDescs },
	{ "playerrocket", "ped", MI_COP, awc(aPlayerWithRocketAnimations), aStdAnimDescs },
	{ "player1armed", "ped", MI_COP, awc(aPlayer1ArmedAnimations), aStdAnimDescs },
	{ "player2armed", "ped", MI_COP, awc(aPlayer2ArmedAnimations), aStdAnimDescs },
	{ "playerBBBat", "ped", MI_COP, awc(aPlayerBBBatAnimations), aStdAnimDescs },
	{ "playercsaw", "ped", MI_COP, awc(aPlayerChainsawAnimations), aStdAnimDescs },
	{ "shuffle", "ped", MI_COP, awc(aShuffleAnimations), aStdAnimDescs },
	{ "oldman", "ped", MI_COP, awc(aOldAnimations), aStdAnimDescs },
	{ "gang1", "ped", MI_COP, awc(aGang1Animations), aStdAnimDescs },
	{ "gang2", "ped", MI_COP, awc(aGang2Animations), aStdAnimDescs },
	{ "fatman", "ped", MI_COP, awc(aFatAnimations), aStdAnimDescs },
	{ "oldfatman", "ped", MI_COP, awc(aOldFatAnimations), aStdAnimDescs },
	{ "jogger", "ped", MI_COP, awc(aJoggerAnimations), aStdAnimDescs },
	{ "woman", "ped", MI_COP, awc(aStdWomanAnimations), aStdAnimDescs },
	{ "shopping", "ped", MI_COP, awc(aWomanShopAnimations), aStdAnimDescs },
	{ "busywoman", "ped", MI_COP, awc(aBusyWomanAnimations), aStdAnimDescs },
	{ "sexywoman", "ped", MI_COP, awc(aSexyWomanAnimations), aStdAnimDescs },
	{ "fatwoman", "ped", MI_COP, awc(aFatWomanAnimations), aStdAnimDescs },
	{ "oldwoman", "ped", MI_COP, awc(aOldWomanAnimations), aStdAnimDescs },
	{ "jogwoman", "ped", MI_COP, awc(aJoggerWomanAnimations), aStdAnimDescs },
	{ "panicchunky", "ped", MI_COP, awc(aPanicChunkyAnimations), aStdAnimDescs },
	{ "skate", "skate", MI_COP, awc(aSkateAnimations), aStdAnimDescs },
#ifdef PC_PLAYER_CONTROLS
	{ "playerback", "ped", MI_COP, awc(aPlayerStrafeBackAnimations), aStdAnimDescs },
	{ "playerleft", "ped", MI_COP, awc(aPlayerStrafeLeftAnimations), aStdAnimDescsSide },
	{ "playerright", "ped", MI_COP, awc(aPlayerStrafeRightAnimations), aStdAnimDescsSide },
	{ "rocketback", "ped", MI_COP, awc(aRocketStrafeBackAnimations), aStdAnimDescs },
	{ "rocketleft", "ped", MI_COP, awc(aRocketStrafeLeftAnimations), aStdAnimDescsSide },
	{ "rocketright", "ped", MI_COP, awc(aRocketStrafeRightAnimations), aStdAnimDescsSide },
	{ "csawback", "ped", MI_COP, awc(aChainsawStrafeBackAnimations), aStdAnimDescs },
	{ "csawleft", "ped", MI_COP, awc(aChainsawStrafeLeftAnimations), aStdAnimDescsSide },
	{ "csawright", "ped", MI_COP, awc(aChainsawStrafeRightAnimations), aStdAnimDescsSide },
#endif
};
#undef awc

void
CAnimManager::Initialise(void)
{
	ms_numAnimations = 0;
	ms_numAnimBlocks = 0;
	ms_animCache.Init(25);
}

void
CAnimManager::Shutdown(void)
{
	int i;

	for(i = 0; i < NUMANIMBLOCKS; i++)
		CStreaming::RemoveAnim(i);

	for(i = 0; i < ms_numAnimations; i++)
		ms_aAnimations[i].Shutdown();

	ms_animCache.Shutdown();

	delete[] ms_aAnimAssocGroups;
	ms_aAnimAssocGroups = nil;
}

void
CAnimManager::UncompressAnimation(CAnimBlendHierarchy *hier)
{
	if(hier->keepCompressed){
		if(hier->totalLength == 0.0f)
			hier->CalcTotalTimeCompressed();
	}else{
		if(!hier->compressed){
			if(hier->linkPtr){
				hier->linkPtr->Remove();
				ms_animCache.head.Insert(hier->linkPtr);
			}
		}else{
			CLink<CAnimBlendHierarchy*> *link = ms_animCache.Insert(hier);
			if(link == nil){
				CAnimBlendHierarchy *lastHier = ms_animCache.tail.prev->item;
				lastHier->RemoveUncompressedData();
				ms_animCache.Remove(ms_animCache.tail.prev);
				lastHier->linkPtr = nil;
				link = ms_animCache.Insert(hier);
			}
			hier->linkPtr = link;
			hier->Uncompress();
		}
	}
}

void
CAnimManager::RemoveFromUncompressedCache(CAnimBlendHierarchy *hier)
{
	if(hier->linkPtr){
		ms_animCache.Remove(hier->linkPtr);
		hier->linkPtr = nil;
	}
}

CAnimBlock*
CAnimManager::GetAnimationBlock(const char *name)
{
	int i;

	for(i = 0; i < ms_numAnimBlocks; i++)
		if(strcasecmp(ms_aAnimBlocks[i].name, name) == 0)
			return &ms_aAnimBlocks[i];
	return nil;
}

int32
CAnimManager::GetAnimationBlockIndex(const char *name)
{
	int i;

	for(i = 0; i < ms_numAnimBlocks; i++)
		if(strcasecmp(ms_aAnimBlocks[i].name, name) == 0)
			return i;
	return -1;
}

int32
CAnimManager::RegisterAnimBlock(const char *name)
{
	if(name == nil || strlen(name) >= MAX_ANIMBLOCK_NAME)
		return -1;
	CAnimBlock *animBlock = GetAnimationBlock(name);
	if(animBlock == nil){
		if(ms_numAnimBlocks >= NUMANIMBLOCKS)
			return -1;
		animBlock = &ms_aAnimBlocks[ms_numAnimBlocks++];
		memset(animBlock, 0, sizeof(*animBlock));
		strcpy(animBlock->name, name);
		animBlock->numAnims = 0;
	}
	return animBlock - ms_aAnimBlocks;
}

int32
CAnimManager::GetNumRefsToAnimBlock(int32 block)
{
	return ms_aAnimBlocks[block].refCount;
}

void
CAnimManager::AddAnimBlockRef(int32 block)
{
	ms_aAnimBlocks[block].refCount++;
}

void
CAnimManager::RemoveAnimBlockRefWithoutDelete(int32 block)
{
	ms_aAnimBlocks[block].refCount--;
}

void
CAnimManager::RemoveAnimBlockRef(int32 block)
{
	ms_aAnimBlocks[block].refCount--;
	if(ms_aAnimBlocks[block].refCount == 0)
		CStreaming::RemoveAnim(block);
}

void
CAnimManager::RemoveAnimBlock(int32 block)
{
	int i;
	CAnimBlock *animblock;

	animblock = &ms_aAnimBlocks[block];
	debug("Removing ANIMS %s\n", animblock->name);
	for(i = 0; i < NUM_ANIM_ASSOC_GROUPS; i++)
		if(ms_aAnimAssocGroups[i].animBlock == animblock)
			ms_aAnimAssocGroups[i].DestroyAssociations();
	for(i = 0; i < animblock->numAnims; i++)
		ms_aAnimations[animblock->firstIndex + i].Shutdown();
	animblock->isLoaded = false;
	animblock->refCount = 0;
}

CAnimBlendHierarchy*
CAnimManager::GetAnimation(const char *name, CAnimBlock *animBlock)
{
	int i;
	CAnimBlendHierarchy *hier = &ms_aAnimations[animBlock->firstIndex];

	for(i = 0; i < animBlock->numAnims; i++){
		if(strcasecmp(hier->name, name) == 0)
			return hier;
		hier++;
	}
	return nil;
}

const char*
CAnimManager::GetAnimGroupName(AssocGroupId groupId)
{
	return ms_aAnimAssocDefinitions[groupId].name;
}

CAnimBlendAssociation*
CAnimManager::CreateAnimAssociation(AssocGroupId groupId, AnimationId animId)
{
	return ms_aAnimAssocGroups[groupId].CopyAnimation(animId);
}

CAnimBlendAssociation*
CAnimManager::GetAnimAssociation(AssocGroupId groupId, AnimationId animId)
{
	return ms_aAnimAssocGroups[groupId].GetAnimation(animId);
}

CAnimBlendAssociation*
CAnimManager::GetAnimAssociation(AssocGroupId groupId, const char *name)
{
	return ms_aAnimAssocGroups[groupId].GetAnimation(name);
}

CAnimBlendAssociation*
CAnimManager::AddAnimation(RpClump *clump, AssocGroupId groupId, AnimationId animId)
{
	CAnimBlendAssociation *anim = CreateAnimAssociation(groupId, animId);
	CAnimBlendClumpData *clumpData = *RPANIMBLENDCLUMPDATA(clump);
	if(anim->IsMovement()){
		CAnimBlendAssociation *syncanim = nil;
		CAnimBlendLink *link;
		for(link = clumpData->link.next; link; link = link->next){
			syncanim = CAnimBlendAssociation::FromLink(link);
			if(syncanim->IsMovement())
				break;
		}
		if(link){
			anim->SyncAnimation(syncanim);
			anim->flags |= ASSOC_RUNNING;
		}else
			anim->Start(0.0f);
	}else
		anim->Start(0.0f);

	clumpData->link.Prepend(&anim->link);
	return anim;
}

CAnimBlendAssociation*
CAnimManager::AddAnimationAndSync(RpClump *clump, CAnimBlendAssociation *syncanim, AssocGroupId groupId, AnimationId animId)
{
	CAnimBlendAssociation *anim = CreateAnimAssociation(groupId, animId);
	CAnimBlendClumpData *clumpData = *RPANIMBLENDCLUMPDATA(clump);
	if (anim->IsMovement() && syncanim){
		anim->SyncAnimation(syncanim);
		anim->flags |= ASSOC_RUNNING;
	}else
		anim->Start(0.0f);

	clumpData->link.Prepend(&anim->link);
	return anim;
}

CAnimBlendAssociation*
CAnimManager::BlendAnimation(RpClump *clump, AssocGroupId groupId, AnimationId animId, float delta)
{
	int removePrevAnim = 0;
	CAnimBlendClumpData *clumpData = *RPANIMBLENDCLUMPDATA(clump);
	CAnimBlendAssociation *anim = GetAnimAssociation(groupId, animId);
	bool isMovement = anim->IsMovement();
	bool isPartial = anim->IsPartial();
	CAnimBlendLink *link;
	CAnimBlendAssociation *found = nil, *movementAnim = nil;
	for(link = clumpData->link.next; link; link = link->next){
		anim = CAnimBlendAssociation::FromLink(link);
		if(isMovement && anim->IsMovement())
			movementAnim = anim;
		if(anim->animId == animId)
			found = anim;
		else{
			if(isPartial == anim->IsPartial()){
				if(anim->blendAmount > 0.0f){
					float blendDelta = -delta*anim->blendAmount;
					if(blendDelta < anim->blendDelta || !isPartial)
						anim->blendDelta = blendDelta;
				}else{
					anim->blendDelta = -1.0f;
				}
				anim->flags |= ASSOC_DELETEFADEDOUT;
				removePrevAnim = 1;
			}
		}
	}
	if(found){
		found->blendDelta = (1.0f - found->blendAmount)*delta;
		if(!found->IsRunning() && found->currentTime == found->hierarchy->totalLength)
			found->Start(0.0f);
	}else{
		found = AddAnimationAndSync(clump, movementAnim, groupId, animId);
		if(!removePrevAnim && !isPartial){
			found->blendAmount = 1.0f;
			return found;
		}
		found->blendAmount = 0.0f;
		found->blendDelta = delta;
	}
	UncompressAnimation(found->hierarchy);
	return found;
}

bool
CAnimManager::LoadAnimFiles(void)
{
	if(ms_aAnimAssocGroups != nil)
		return false;
	ms_aAnimAssocGroups = new(std::nothrow) CAnimBlendAssocGroup[NUM_ANIM_ASSOC_GROUPS];
	if(ms_aAnimAssocGroups == nil)
		return false;
	if(!LoadAnimFile("ANIM\\PED.IFP")){
		delete[] ms_aAnimAssocGroups;
		ms_aAnimAssocGroups = nil;
		return false;
	}
#ifdef GTA_OGC
	BootLog("  assoc groups");
#endif
	CreateAnimAssocGroups();
	return true;
}

void
CAnimManager::CreateAnimAssocGroups(void)
{
	int i, j;

	for(i = 0; i < NUM_ANIM_ASSOC_GROUPS; i++){
#ifdef GTA_OGC
		{
			char line[64];
			snprintf(line, sizeof(line), "  assoc %d/%d", i, NUM_ANIM_ASSOC_GROUPS);
			BootLog(line);
		}
#endif
		CAnimBlock *block = GetAnimationBlock(ms_aAnimAssocDefinitions[i].blockName);
		if(block == nil || !block->isLoaded || ms_aAnimAssocGroups[i].assocList)
			continue;

		CBaseModelInfo *mi = CModelInfo::GetModelInfo(ms_aAnimAssocDefinitions[i].modelIndex);
		RpClump *clump = (RpClump*)mi->CreateInstance();
#ifdef GTA_OGC
		BootLog(clump ? "   inst ok" : "   inst NIL");
		// CreateInstance can fail — the model may not be resident, or its load
		// may have failed — and everything below dereferences the clump. Stock
		// code assumes it always succeeds, which on a console with a real
		// memory ceiling means a nil deref during boot rather than one missing
		// animation group.
		if(clump == nil)
			continue;
#endif
		RpAnimBlendClumpInit(clump);
		CAnimBlendAssocGroup *group = &ms_aAnimAssocGroups[i];
		const AnimAssocDefinition *def = &ms_aAnimAssocDefinitions[i];
		group->groupId = i;
		group->firstAnimId = def->animDescs[0].animId;
		group->CreateAssociations(def->blockName, clump, def->animNames, def->numAnims);
#ifdef GTA_OGC
		BootLog("   assocs made");
#endif
		for(j = 0; j < group->numAssociations; j++)
			// GetAnimation(i) in III (but it's in LoadAnimFiles), GetAnimation(group->animDesc[j].animId) in VC
			group->GetAnimation(def->animDescs[j].animId)->flags |= def->animDescs[j].flags;
		if(IsClumpSkinned(clump))
			RpClumpForAllAtomics(clump, AtomicRemoveAnimFromSkinCB, nil);
		RpClumpDestroy(clump);
	}
}

bool
CAnimManager::LoadAnimFile(const char *filename)
{
#ifdef GTA_OGC
	BootLog("  ifp open");
#endif
	RwStream *stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	if(stream == nil)
		return false;
	bool success = LoadAnimFile(stream, true);
#ifdef GTA_OGC
	BootLog(success ? "  ifp parsed" : "  ifp parse FAILED");
#endif
	RwStreamClose(stream, nil);
	return success;
}

struct IfpHeader
{
	char ident[4];
	uint32 rawSize;
	uint32 paddedSize;
};

struct IfpReader
{
	RwStream *stream;
	uint64 position;

	IfpReader(RwStream *stream) : stream(stream), position(0) {}

	bool Read(void *data, uint32 size, uint64 end)
	{
		if(position > end || size > end - position ||
		   RwStreamRead(stream, data, size) != size)
			return false;
		position += size;
		return true;
	}

	bool ReadHeader(IfpHeader &header, const char *ident, uint64 parentEnd, uint64 &chunkEnd)
	{
		uint8 data[8];
		if(!Read(data, sizeof(data), parentEnd))
			return false;
		memcpy(header.ident, data, sizeof(header.ident));
		if(ident && memcmp(header.ident, ident, sizeof(header.ident)) != 0)
			return false;
		header.rawSize = ReadLE32(&data[4]);
		if(header.rawSize > 0xFFFFFFFC)
			return false;
		header.paddedSize = (header.rawSize + 3) & ~3;
		if(position > parentEnd || header.paddedSize > parentEnd - position)
			return false;
		chunkEnd = position + header.paddedSize;
		return true;
	}
};

static bool
ReadIfpData(IfpReader &reader, void *data, const IfpHeader &header, uint32 capacity, uint64 end)
{
	if(header.paddedSize > capacity || reader.position + header.paddedSize != end)
		return false;
	memset(data, 0, capacity);
	return reader.Read(data, header.paddedSize, end) && reader.position == end;
}

static bool
ReadIfpFloats(IfpReader &reader, float *values, uint32 count, uint64 end)
{
	uint8 data[11*sizeof(float)];
	if(count > 11 || !reader.Read(data, count*sizeof(float), end))
		return false;
	for(uint32 i = 0; i < count; i++)
		values[i] = ReadLEFloat32(&data[i*sizeof(float)]);
	return true;
}

static bool
FitsCompressedIfpValue(float value, float scale, float bias = 0.0f)
{
	float encoded = value*scale + bias;
	return encoded >= INT16_MIN && encoded <= INT16_MAX;
}

static bool
FitsCompressedIfpRotationValue(float value)
{
	float encoded = value*4096.0f;
	return encoded >= -INT16_MAX && encoded <= INT16_MAX;
}

static bool
FitsCompressedIfpFrame(const CQuaternion &rotation, const CVector *translation, float time)
{
	if(!FitsCompressedIfpRotationValue(rotation.x) ||
	   !FitsCompressedIfpRotationValue(rotation.y) ||
	   !FitsCompressedIfpRotationValue(rotation.z) ||
	   !FitsCompressedIfpRotationValue(rotation.w) ||
	   !FitsCompressedIfpValue(time, 60.0f, 0.5f))
		return false;
	return translation == nil ||
	       (FitsCompressedIfpValue(translation->x, 1024.0f) &&
	        FitsCompressedIfpValue(translation->y, 1024.0f) &&
	        FitsCompressedIfpValue(translation->z, 1024.0f));
}

bool
CAnimManager::LoadAnimFile(RwStream *stream, bool compress, char (*uncompressedAnims)[32], uint64 maxBytes)
{
	IfpReader reader(stream);
	IfpHeader anpk, info, name, dgan, cpan, anim, keys;
	uint64 anpkEnd, infoEnd, nameEnd, dganEnd, cpanEnd, animEnd, keysEnd;
	char buf[256];
	char blockName[MAX_ANIMBLOCK_NAME];
	int32 fileNumAnims;
	int32 firstIndex = 0;
	int32 createdHierarchies = 0;
	int32 oldNumAnimBlocks = ms_numAnimBlocks;
	int32 oldNumAnimations = ms_numAnimations;
	CAnimBlock *animBlock = nil;
	bool newBlock = false;
	int j, k, l;
	float fbuf[11];

	if(stream == nil ||
	   !reader.ReadHeader(anpk, "ANPK", maxBytes, anpkEnd) ||
	   !reader.ReadHeader(info, "INFO", anpkEnd, infoEnd) ||
	   info.rawSize <= 4 || !ReadIfpData(reader, buf, info, sizeof(buf), infoEnd) ||
	   memchr(buf + 4, '\0', info.rawSize - 4) == nil ||
	   strlen(buf + 4) >= sizeof(blockName))
		goto fail;
	fileNumAnims = (int32)ReadLE32(buf);
	if(fileNumAnims < 0)
		goto fail;
	strcpy(blockName, buf + 4);
	animBlock = GetAnimationBlock(blockName);
	if(animBlock){
		if(animBlock->isLoaded ||
		   (animBlock->numAnims != 0 && animBlock->numAnims != fileNumAnims))
			goto fail;
		if(animBlock->numAnims == 0){
			if(fileNumAnims > NUMANIMATIONS - ms_numAnimations)
				goto fail;
			firstIndex = ms_numAnimations;
		}else{
			firstIndex = animBlock->firstIndex;
			if(firstIndex < 0 || firstIndex > NUMANIMATIONS ||
			   fileNumAnims > NUMANIMATIONS - firstIndex)
				goto fail;
		}
	}else{
		if(ms_numAnimBlocks >= NUMANIMBLOCKS ||
		   fileNumAnims > NUMANIMATIONS - ms_numAnimations)
			goto fail;
		animBlock = &ms_aAnimBlocks[ms_numAnimBlocks];
		newBlock = true;
		firstIndex = ms_numAnimations;
	}
	for(j = 0; j < fileNumAnims; j++)
		if(ms_aAnimations[firstIndex + j].sequences != nil ||
		   ms_aAnimations[firstIndex + j].linkPtr != nil)
			goto fail;

	for(j = 0; j < fileNumAnims; j++){
#ifdef GTA_OGC
		if(j % 100 == 0){
			char line[64];
			snprintf(line, sizeof(line), "  anim %d/%d", j, fileNumAnims);
			BootLog(line);
		}
#endif
		CAnimBlendHierarchy *hier = &ms_aAnimations[firstIndex + j];
		createdHierarchies++;
		hier->totalLength = 0.0f;
		hier->keepCompressed = false;

		if(!reader.ReadHeader(name, "NAME", anpkEnd, nameEnd) ||
		   name.rawSize == 0 || !ReadIfpData(reader, buf, name, sizeof(buf), nameEnd) ||
		   memchr(buf, '\0', name.rawSize) == nil || strlen(buf) >= sizeof(hier->name))
			goto fail;
		hier->SetName(buf);

#ifdef ANIM_COMPRESSION
		bool compressHier = compress;
#else
		bool compressHier = false;
#endif
		if (uncompressedAnims) {
			for (int i = 0; uncompressedAnims[i][0]; i++) {
				if (!CGeneral::faststricmp(uncompressedAnims[i], hier->name)){
					debug("Loading %s uncompressed\n", hier->name);
					compressHier = false;
				}
			}
		}

		hier->compressed = compressHier;
		hier->keepCompressed = false;

		// VC's ped.ifp writes 8-byte DGAN INFOs; only the first int matters.
		if(!reader.ReadHeader(dgan, "DGAN", anpkEnd, dganEnd) ||
		   !reader.ReadHeader(info, "INFO", dganEnd, infoEnd) ||
		   info.rawSize < 4 || !ReadIfpData(reader, buf, info, sizeof(buf), infoEnd))
			goto fail;
		int32 numSequences = (int32)ReadLE32(buf);
		if(numSequences < 0 || numSequences > INT16_MAX ||
		   (uint64)(uint32)numSequences * 48 > dganEnd - reader.position)
			goto fail;
		hier->numSequences = (int16)numSequences;
		if(numSequences > 0){
			hier->sequences = new(std::nothrow) CAnimBlendSequence[numSequences];
			if(hier->sequences == nil){
				hier->numSequences = 0;
				goto fail;
			}
		}

		CAnimBlendSequence *seq = hier->sequences;
		for(k = 0; k < hier->numSequences; k++, seq++){
			// 32 = III-style, 44 = +boneTag, 48 = VC ped.ifp (+4 unknown).
			if(!reader.ReadHeader(cpan, "CPAN", dganEnd, cpanEnd) ||
			   !reader.ReadHeader(anim, "ANIM", cpanEnd, animEnd) ||
			   (anim.rawSize != 32 && anim.rawSize != 44 && anim.rawSize != 48) ||
			   !ReadIfpData(reader, buf, anim, sizeof(buf), animEnd) ||
			   memchr(buf, '\0', 24) == nil)
				goto fail;
			int numFrames = (int32)ReadLE32(buf+28);
			if(numFrames < 0)
				goto fail;
			seq->SetName(buf);
			if(anim.rawSize >= 44)
				seq->SetBoneTag((int32)ReadLE32(buf+40));
			if(numFrames == 0){
				if(reader.position != cpanEnd)
					goto fail;
				continue;
			}

			bool hasScale = false;
			bool hasTranslation = false;
			uint32 floatsPerFrame;
			if(!reader.ReadHeader(keys, nil, cpanEnd, keysEnd) ||
			   (memcmp(keys.ident, "KRTS", 4) != 0 &&
			    memcmp(keys.ident, "KRT0", 4) != 0 &&
			    memcmp(keys.ident, "KR00", 4) != 0))
				goto fail;
			if(memcmp(keys.ident, "KRTS", 4) == 0){
				hasScale = true;
				floatsPerFrame = 11;
			}else if(memcmp(keys.ident, "KRT0", 4) == 0){
				hasTranslation = true;
				floatsPerFrame = 8;
			}else
				floatsPerFrame = 5;
			uint64 keyBytes = (uint64)(uint32)numFrames * floatsPerFrame * sizeof(float);
			if(keyBytes > UINT32_MAX || keys.rawSize != keyBytes ||
			   !seq->SetNumFrames(numFrames, hasScale || hasTranslation, compressHier))
				goto fail;
			if(strstr(seq->name, "L Toe"))
				debug("anim %s has toe keyframes\n", hier->name); // BUG: seq->name

			for(l = 0; l < numFrames; l++){
				if(hasScale){
					if(!ReadIfpFloats(reader, fbuf, 11, keysEnd))
						goto fail;
					CQuaternion rot(fbuf[0], fbuf[1], fbuf[2], fbuf[3]);
					rot.Invert();
					CVector trans(fbuf[4], fbuf[5], fbuf[6]);

					if(compressHier){
						if(!FitsCompressedIfpFrame(rot, &trans, fbuf[10]))
							goto fail;
						KeyFrameTransCompressed *kf = (KeyFrameTransCompressed*)seq->GetKeyFrameCompressed(l);
						kf->SetRotation(rot);
						kf->SetTranslation(trans);
						// scaling ignored
						kf->SetTime(fbuf[10]);	// absolute time here
					}else{
						KeyFrameTrans *kf = (KeyFrameTrans*)seq->GetKeyFrame(l);
						kf->rotation = rot;
						kf->translation = trans;
						// scaling ignored
						kf->deltaTime = fbuf[10];	// absolute time here
					}
				}else if(hasTranslation){
					if(!ReadIfpFloats(reader, fbuf, 8, keysEnd))
						goto fail;
					CQuaternion rot(fbuf[0], fbuf[1], fbuf[2], fbuf[3]);
					rot.Invert();
					CVector trans(fbuf[4], fbuf[5], fbuf[6]);

					if(compressHier){
						if(!FitsCompressedIfpFrame(rot, &trans, fbuf[7]))
							goto fail;
						KeyFrameTransCompressed *kf = (KeyFrameTransCompressed*)seq->GetKeyFrameCompressed(l);
						kf->SetRotation(rot);
						kf->SetTranslation(trans);
						kf->SetTime(fbuf[7]);	// absolute time here
					}else{
						KeyFrameTrans *kf = (KeyFrameTrans*)seq->GetKeyFrame(l);
						kf->rotation = rot;
						kf->translation = trans;
						kf->deltaTime = fbuf[7];	// absolute time here
					}
				}else{
					if(!ReadIfpFloats(reader, fbuf, 5, keysEnd))
						goto fail;
					CQuaternion rot(fbuf[0], fbuf[1], fbuf[2], fbuf[3]);
					rot.Invert();

					if(compressHier){
						if(!FitsCompressedIfpFrame(rot, nil, fbuf[4]))
							goto fail;
						KeyFrameCompressed *kf = (KeyFrameCompressed*)seq->GetKeyFrameCompressed(l);
						kf->SetRotation(rot);
						kf->SetTime(fbuf[4]);	// absolute time here
					}else{
						KeyFrame *kf = (KeyFrame*)seq->GetKeyFrame(l);
						kf->rotation = rot;
						kf->deltaTime = fbuf[4];	// absolute time here
					}
				}
			}
			if(reader.position != keysEnd || reader.position != cpanEnd)
				goto fail;
		}
		if(reader.position != dganEnd)
			goto fail;

		if(!compressHier){
			hier->RemoveQuaternionFlips();
			hier->CalcTotalTime();
		}
	}
	if(reader.position != anpkEnd)
		goto fail;
	if(newBlock){
		memset(animBlock, 0, sizeof(*animBlock));
		strcpy(animBlock->name, blockName);
		ms_numAnimBlocks++;
	}
	animBlock->numAnims = fileNumAnims;
	animBlock->firstIndex = firstIndex;
	animBlock->isLoaded = true;
	if(firstIndex + fileNumAnims > ms_numAnimations)
		ms_numAnimations = firstIndex + fileNumAnims;
	debug("Loading ANIMS %s\n", animBlock->name);
	return true;

fail:
	for(j = 0; j < createdHierarchies; j++)
		ms_aAnimations[firstIndex + j].Shutdown();
	ms_numAnimBlocks = oldNumAnimBlocks;
	ms_numAnimations = oldNumAnimations;
	return false;
}

void
CAnimManager::RemoveLastAnimFile(void)
{
	int i;
	ms_numAnimBlocks--;
	ms_numAnimations = ms_aAnimBlocks[ms_numAnimBlocks].firstIndex;
	for(i = 0; i < ms_aAnimBlocks[ms_numAnimBlocks].numAnims; i++)
		ms_aAnimations[ms_aAnimBlocks[ms_numAnimBlocks].firstIndex + i].Shutdown();
	ms_aAnimBlocks[ms_numAnimBlocks].isLoaded = false;
}
