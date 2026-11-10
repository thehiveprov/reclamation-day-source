#pragma once

#include <cstdint>

// Nemeses: the wire's numbers as the engine uses them, without the encoding library (the harness builds without it).
// NemesisService checks them against RequestNemesis / NotifyNemesis / NemesisSpawn with static_asserts.
namespace Nemesis::Wire
{
enum Deed : uint8_t
{
    kDownedYou,
    kWounded,
    kCrippled,
    kComradeKilled,
    kRobbed,
    kGunfight,
    kSurrendered,
    kLetGo,
    kExecuted,
    kThreatened,
    kPaid,
    kHit,
    kStashRobbed,
    kCampCleared,
    kMet,
};

enum DeathFlags : int32_t
{
    kByLocalPlayer = 1 << 0,
    kFinal = 1 << 1,
    kWhileSurrendered = 1 << 2,
    kByOtherPlayer = 1 << 3,
    kLeftLeg = 1 << 4,
    kRightLeg = 1 << 5,
    kLeftArm = 1 << 6,
    kRightArm = 1 << 7,
    kHead = 1 << 8,
};

enum Outcome : uint8_t
{
    kHeKilled,
    kHeDownedYou,
    kHeFled,
    kHeSurrendered,
    kYouLeft,
    kBackedDown,
    kCancelled,
};

enum Question : uint8_t
{
    kAskBoss,
    kAskCamp,
    kAskStash,
    kAskKin,
    kAskHabits,
    kAskWhy,
    kThreaten,
    kPay,
    kLetHimGo,
    kFinishHim,
    kClose,
    kOpen, // the card, nothing asked
};

enum SpawnKind : uint8_t
{
    kAmbush,
    kCallOut,
    kCrew,
    kHunters,
    kSighting,
    kCamp,
    kPatrol,
};

enum Order : uint8_t
{
    kSurrender,
    kFlee,
    kCalm,
    kAttack,
    kLetGoRuns,   // NotifyNemesis::kLetGo
    kLetGoFights, // NotifyNemesis::kLetGoFight
};

enum NoticeStyle : uint8_t
{
    kNormal,
    kRises,
    kDeath,
    kWorld,
};

enum Injury : uint8_t
{
    kLimp = 1 << 0,
    kBadArm = 1 << 1,
    kBurned = 1 << 2,
    kOneEye = 1 << 3,
    kRinging = 1 << 4,
};
} // namespace Nemesis::Wire
