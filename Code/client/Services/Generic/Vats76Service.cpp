#include <Services/Vats76Service.h>

#if TP_FALLOUT4

#include <World.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <NodeTools.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESObjectWEAP.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Interface/HudMarker.h>
#include <Interface/IMenu.h>
#include <Interface/Scaleform.h>
#include <Interface/UI.h>
#include <Games/Memory.h>
#include <Misc/BSFixedString.h>
#include <MiniJson.h>
#include <Projectiles/Projectile.h>
#include <TESObjectREFR.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESNPC.h>
#include <Games/Fallout4/NemesisGame.h>
#include <WorldRays.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

// The game's side of it, read from the exe (Reclamation Day ids: CommonLibF4's + 1; docs/VATS76.md)
namespace
{
// The hit chance of a part of a target for the player's weapon, as the VATS menu asks it: (target, weapon, part or null for
// the whole target, how visible the target is 0..1)
constexpr uint32_t kHitChance = 984571;
// An actor's equipped weapon: (actor, out instance, equip index)
constexpr uint32_t kGetWeaponInstance = 1277202;
// The VATS Action Points of an attack with a weapon, before perks: (instance, true, false)
constexpr uint32_t kAttackActionPoints = 85996;
// How many rounds a VATS attack with this weapon fires: (instance)
constexpr uint32_t kBurstRounds = 1076922;
// Perks' entry points: (entry point, owner, its arguments..., float* value)
constexpr uint32_t kHandleEntryPoint = 714337;
constexpr uint32_t kEntryVatsAttackCost = 0x4F;
// A direction as the launch data's angles: (direction, z angle, x angle)
constexpr uint32_t kDirectionToAngles = 1306270;
// Whether the player could see a target, as VATS asks it: (player, target, out)
constexpr uint32_t kCanSee = 449776;
// Whether a handle's a target VATS could take: (handle, with the race's own rule too; the plugin passes true)
constexpr uint32_t kVatsTargetable = 391949;
// The game's lists of live projectiles (+0x8 and +0x20, BSTArrays of Projectile*, under the recursive lock at +0x50). Only a
// GrenadeProjectile answers yes to vtable slot 0xC7 (a thrown grenade or a placed mine; every other kind has xor al,al)
constexpr uint32_t kProjectileManager = 338228;
constexpr uintptr_t kProjectileLists[] = {0x8, 0x20};
constexpr uintptr_t kListData = 0x0;
constexpr uintptr_t kListSize = 0x10;
constexpr uintptr_t kProjectileLockOwner = 0x50;
constexpr uintptr_t kProjectileLockCount = 0x54;
constexpr size_t kIsGrenadeProjectile = 0xC7;
// Whether a mine or grenade's handle is one VATS could take: (handle, false)
constexpr uint32_t kExplosiveTargetable = 989490;
// NiAVObject's world bound: its centre, and its radius after it
constexpr uintptr_t kWorldBoundCentre = 0xB0;
constexpr uintptr_t kWorldBoundRadius = 0xBC;
// An actor's body part data: (actor)
constexpr uint32_t kGetBodyPartData = 1485197;
// Whether a limb is off: (actor, limb)
constexpr uint32_t kIsDismembered = 957918;
constexpr uint32_t kIsHostileToActor = 1148687;
constexpr uint32_t kIsDogmeat = 519764;
constexpr uint32_t kIsPlayerTeammate = 1102019;
// A reference's linked reference through a keyword: (ref, keyword) -> the ref or null. A settler is linked to their
// settlement's workbench through WorkshopItemKeyword, which is how the plugin and the game tell them apart
constexpr uint32_t kGetLinkedRef = 897288; // CommonLibF4: TESObjectREFR::GetLinkedRef, id 897287
constexpr uint32_t kWorkshopItemKeyword = 0x54BA6;
// Stops the player's weapon firing: (player)
constexpr uint32_t kStopFiring = 1440502;
// A message at the top left of the HUD: (text, sound, throttle, warning)
constexpr uint32_t kShowHudMessage = 1163006;

// The HUD's meters kept up whatever their own rules say: HUDMenuUtils::SetForceShowMeter (meter, show) sends the HUD a
// HDT_SET_FORCE_SHOW_METER HUDData, which each meter takes when the number is its own (+0x328: the Action Points meter 0,
// the critical meter 1). The game's VATS forces these four as it opens and lets them go as it closes (358301 +0x211, +0x7B)
constexpr uint32_t kForceShowMeter = 227519;
constexpr uint32_t kVatsMeters[] = {4, 0, 2, 1};
// The critical meter: the player's charge and banked strikes, and the game setting the charge fills to
// (fVATSCriticalChargeLimit's value)
constexpr uintptr_t kCriticalCharge = 0xD48;
constexpr uintptr_t kCriticalBanked = 0xD4C;
constexpr uint32_t kCriticalChargeLimit = 695443;
// What the game's VATS does with each attack as it's launched (651348): one that will hit gives the meter its charge for
// the weapon (197436: the reward for the weapon and Luck, the Four Leaf Clover entry point, the HUD's note); a critical one
// spends a strike (702171: the full charge, else a banked one, then the refund perks) and each of its projectiles carries
// 0x1000000 at +0x260, which makes its hits critical (the critical damage 470088 reads it when the roll didn't)
constexpr uint32_t kAddCriticalCharge = 197437;
constexpr uint32_t kSpendCritical = 702172;
constexpr uintptr_t kProjectileFlags = 0x260;
constexpr uint64_t kProjectileCritical = 0x1000000;
// The target glow (SelectionGlow has the manager): a reference counted target over a 3D, kept to the segments of the given
// body part types as the game's VATS keeps a part's glow (1413608): (target, type, 0xFF for all of its subsegments). Added
// and removed one at a time; on in the world with Enable(false, 0) (true is the VATS menu's own drawing)
constexpr uint32_t kGlowConstructor = 820922;
constexpr uint32_t kGlowDestructor = 1073051;
constexpr uint32_t kGlowAddSegment = 214442;
constexpr uint32_t kGlowAddTarget = 126856;
constexpr uint32_t kGlowRemoveTarget = 1335847;
constexpr uint32_t kGlowEnable = 499746;
constexpr uint32_t kGlowDisable = 845362;
constexpr uint32_t kGlowColour = 403513;
// how many targets the manager has
constexpr uint32_t kGlowCount = 824395;
constexpr size_t kGlowSize = 0xA8;
constexpr size_t kGlowGeometryCount = 0x30;
constexpr uint8_t kGlowAllSubsegments = 0xFF;
// The gameplay HUD colour as the game has it now (a NiColor, 0..1; the colour type 2 of HUD shader targets)
constexpr uint32_t kHudColour = 953745;
// TESObjectREFR's 3D (vtable slot)
constexpr size_t kGet3D = 0x8C;

// --- The game's VATS perks. A perk's entries ask GetVATSMode and GetVATSValue whether this is VATS and what's being shot at,
// so those two answer for our attacks, only while a perk is being asked about one (AttackScope on that thread).
constexpr uint32_t kGetVatsMode = 462355;
constexpr uint32_t kGetVatsValue = 990948;
// A form's place in a form list, negative when it isn't in: (list, form)
constexpr uint32_t kFormListIndex = 574357;
// The source of TESDeathEvent
constexpr uint32_t kDeathEvents = 1465691;
// The VATS singleton; +0x40 is non-zero while the game's own VATS is playing an attack
constexpr uint32_t kVatsSingleton = 570122;
constexpr uintptr_t kVatsPlaying = 0x40;
// HitData's attack damage (the perks' damage: (hit data)), called as a melee hit, a projectile hit and an explosion's hit
// are set up
constexpr uint32_t kAttackDamage = 1364262;
constexpr uint32_t kMeleeHitSetup = 1330651;
constexpr uint32_t kMeleeHitDamageSite = 0x5EF;
constexpr uint32_t kProjectileHitSetup = 1327604;
constexpr uint32_t kProjectileHitDamageSite = 0x1E5;
constexpr uint32_t kExplosionHitSetup = 568164;
constexpr uint32_t kExplosionHitDamageSite = 0x16C;
// HitData's fields
constexpr uintptr_t kHitAttacker = 0x40;
constexpr uintptr_t kHitVictim = 0x44;
constexpr uintptr_t kHitWeapon = 0x58;
constexpr uintptr_t kHitVatsCommand = 0x78;
constexpr uintptr_t kHitBaseDamage = 0x90;
constexpr uintptr_t kHitTotalDamage = 0x98;
constexpr uintptr_t kHitFlags = 0xC4;
constexpr uint32_t kHitNotAnAttack = 0x7000;
// An explosion being set up: (explosion, its data). The game gives a shot explosive the Demolition Expert entry here, only
// inside its own VATS; the explosion's damage, and in the data who it's for and the projectile it came from. That projectile
// carries a flag once it's been shot
constexpr uint32_t kExplosionSetup = 62346;
constexpr uintptr_t kExplosionDamage = 0x544;
constexpr uintptr_t kExplosionOwner = 0x10;
constexpr uintptr_t kExplosionSource = 0x20;
constexpr size_t kAsProjectile = 0xB6;
constexpr uint64_t kProjectileShotExplosive = 0x2000000000;
// --- A melee attack. The game's VATS moves the attacker next to the target: 45766 (attacker**, target**, how near) takes a
// spot from 907676 (its target's own position when there's none, so we ask first), moves there and turns to face them.
constexpr uint32_t kMeleeTeleport = 45767;
constexpr uint32_t kFindMeleeSpot = 907677;
constexpr uint32_t kMeleeSpotFinder = 358427;
// How far the weapon reaches: (actor, instance); the jump lands at half of it, as the plugin does
constexpr uint32_t kGetMeleeReach = 347136;
constexpr float kLungeLandAt = 0.5f;
// fVATSMeleeMaxDistance's value, what Blitz adds to
constexpr uint32_t kVatsMeleeMaxDistance = 787413;
constexpr uintptr_t kPlayerWeaponState = 0x134;
constexpr uint8_t kWeaponStateMask = 0xE;
constexpr uint8_t kWeaponStateDrawn = 6;
// A melee hit's start: (attacker, equip index, hit), called from the actor message dispatcher
constexpr uint32_t kMessageDispatcher = 1546752;
constexpr uint32_t kMeleeHitSite = 0x7DB;
constexpr uint32_t kMeleeHit = 216551;
// The chance a hit's critical, read as its critical damage is worked out; a readied critical strike makes it certain
constexpr uint32_t kCriticalDamage = 470089;
constexpr uint32_t kCriticalChanceSite = 0xAA;
constexpr uint32_t kCriticalChance = 1133006;
constexpr float kCertainCritical = 10000.f;
// How long a jump's distance counts for Blitz's damage
constexpr auto kLungeLasts = std::chrono::milliseconds(1500);

// --- The Mysterious Stranger, as the game's VATS has him: 602086 (VATS starting to play its attacks) rolls for him and puts
// him in, 430566 (an attack played) brings him on at a target low on health, 602086 again (VATS closing) sends him off.
// The VATS default objects (BGSDefaultObject, its form at +0x20): the perk he needs (MysteriousStranger01), the global of
// his odds (VATSStrangerOdds 0.1; ranks 2 and 3 raise it through their quest stage), his image space as he comes
constexpr uint32_t kStrangerPerkObject = 290796;
constexpr uint32_t kStrangerOddsObject = 1152332;
constexpr uint32_t kStrangerImageSpaceObject = 244379;
constexpr uintptr_t kDefaultObjectForm = 0x20;
constexpr uint8_t kFormTypePerk = 0x5F;
constexpr uint8_t kFormTypeGlobal = 0x0B;
// He's only there for a target under iVATSStrangerMaxHP health (150) or under fVATSStrangerMaxHPPerc of it (0.4): the
// settings' values (430566 +0x354 and +0x37B read these)
constexpr uint32_t kStrangerMaxHealth = 1032297;
constexpr uint32_t kStrangerMaxHealthShare = 108357;
// His NPC, Fallout4.esm MysteriousStrangerActor (its package, MysteriousStrangerUseWeaponPackage, shoots the linked ref).
// The game has one of him (MysteriousStrangerRef, the VATS singleton's +0xA0); each visit here is a character of his own,
// so two players' Strangers can be out at once and each is the player's own in co-op
constexpr uint32_t kStrangerNpc = 0x188AA7;
// What the game does to him as he comes on: he turns to the target (1310550), is updated (1434933), the target becomes his
// linked ref (192840, TESObjectREFR::SetLinkedRef) and his package is looked at again (1395257, Actor::EvaluatePackage).
// His AI is reset as he's put in (the actor's flags at +0x43C, 0x20000), as 602086 does before moving him
constexpr uint32_t kFaceTarget = 1310551;
constexpr uint32_t kUpdateActor = 1434934;
constexpr uint32_t kEvaluatePackage = 1395258;
constexpr uintptr_t kActorFlags = 0x43C;
constexpr uint32_t kActorResetAI = 0x20000;
// What 430566 plays as he comes: the image space (on at 2, 1417110; off, 526131), the music sting (PlayMenuSound,
// 1227993), the perk's Vault Boy clip with its sound (the sound form by editor id, 1305215; the clip, 1356846), then the
// rank 3 entry point that fills the critical meter. MUSMysteriousStrangerB (Fallout4.esm's too) is for a kill of his, as
// the plugin plays it
constexpr uint32_t kApplyImageSpace = 1417111;
constexpr uint32_t kRemoveImageSpace = 526132;
constexpr float kStrangerImageSpaceStrength = 2.f;
constexpr uint32_t kSoundFormByName = 1305216;
constexpr uint32_t kShowPerkClip = 1356847;
constexpr uint32_t kEntryStrangerFillsCritical = 0x8C;
constexpr char kStrangerMusicIn[] = "MUSMysteriousStrangerA";
constexpr char kStrangerMusicKill[] = "MUSMysteriousStrangerB";
constexpr char kStrangerClipSound[] = "UIPerkMenuMysteriousStranger";
constexpr char kStrangerClip[] = "Components/VaultBoys/Perks/PerkClip_4c929.swf";
// The game's note that he came, as its VATS closes after he did (602086 +0x35B)
constexpr uint32_t kStrangerVisited = 1518067;
// The plugin's timings: a spot looked for every 0.2 s for 1.5 s; on once he's had 0.3 s and has his 3D (1.5 s at most);
// 8 s at most; gone 1.5 s after a kill of his, at once otherwise. A roll waits for the attack's hit to land
constexpr float kStrangerSpotEvery = 0.2f;
constexpr float kStrangerSpotFor = 1.5f;
constexpr float kStrangerArriveAfter = 0.3f;
constexpr float kStrangerArriveBy = 1.5f;
constexpr float kStrangerStaysFor = 8.f;
constexpr float kStrangerLeavesAfterKill = 1.5f;
constexpr auto kStrangerRollAfter = std::chrono::milliseconds(300);
constexpr size_t kStrangerMaxRolls = 16;
// A character the game has only just placed isn't deleted (the game's still dressing it: 0x1401C20D6, 11-05)
constexpr float kStrangerDeleteAfter = 3.f;
// time for Disable's fade before he's deleted
constexpr float kStrangerLeaveFade = 1.5f;
constexpr float kStrangerSweepEvery = 1.f;
// Where he stands: beside or behind the player (side, back), as the plugin looks, on ground near the player's height, with
// a clear line to the player and to the target
constexpr float kStrangerSpots[][2] = {{96.f, 48.f}, {-96.f, 48.f}, {128.f, 0.f}, {-128.f, 0.f}, {48.f, 120.f}, {-48.f, 120.f}};
constexpr float kStrangerEyeHeight = 96.f;
constexpr float kStrangerGroundReach = 128.f;
constexpr float kStrangerMaxStep = 64.f;
constexpr float kStrangerRayGap = 32.f;

// BGSEntryPoint's
constexpr uint32_t kEntryBlitzReach = 0x97;
constexpr uint32_t kEntryBlitzDamageAtMax = 0x98;
constexpr uint32_t kEntryBlitzBonusFrom = 0x99;
constexpr uint32_t kEntryApKillChance = 0x6B;
constexpr uint32_t kEntryConcentratedFire = 0x6D;
constexpr uint32_t kEntryGunFuTargetsForCritical = 0x72;
constexpr uint32_t kEntryConcentratedFireDamage = 0x87;
constexpr uint32_t kEntryFillCriticalOnApReward = 0x8B;
constexpr uint32_t kEntryShootExplosiveDamage = 0x90;
// + 1 the second target, + 2 the third, + 3 the fourth and on
constexpr uint32_t kEntryGunFuDamage = 0x93;
// Gun Fu's chain of targets ends this long after the last attack
constexpr auto kGunFuChain = std::chrono::seconds(5);

// MCM's keybinds: our critical key (no entry: the jump key, as Fallout 76's Space); modifiers 1 Shift, 2 Ctrl, 4 Alt
constexpr wchar_t kKeybindsFile[] = L"MCM\\Settings\\Keybinds.json";
constexpr char kModName[] = "ReclamationDay";
constexpr char kCriticalKeyId[] = "armCritical";
// How thick each kind of part is on its bones (world units), for its box on the screen, by the body part type
[[nodiscard]] float PartThickness(uint8_t aType) noexcept
{
    switch (aType)
    {
    case 0: return 16.f;        // torso
    case 1: case 5: return 9.f; // head
    case 23: case 24: return 5.f; // feet
    case 6: case 7: case 8: case 9: return 5.5f; // arms
    case 10: case 11: case 12: case 13: case 14: case 15: return 7.f; // legs
    default: return 8.f;
    }
}

// The call sites we go through, each checked against what's called there before it's touched
constexpr uint32_t kVatsButtonHandler = 927100;
constexpr uint32_t kVatsButtonSite = 0x1BB;
constexpr uint32_t kOpenVats = 1313740;
constexpr uint32_t kAttackHandler = 300077;
constexpr uint32_t kAttackHandlerStartSite = 0x1CC;
constexpr uint32_t kAttackHandlerReleaseSites[] = {0x139, 0x24E};
constexpr uint32_t kAttackRearm = 788180;
constexpr uint32_t kAttackRearmStartSite = 0x137;
constexpr uint32_t kAttackStart = 1085821;
constexpr uint32_t kAttackRelease = 818082;
constexpr int32_t kAttackAction = 0x27;

// TESObjectREFR: its base form
constexpr uintptr_t kReferenceBaseForm = 0xE0;
// BGSBodyPartData: its parts' pointers; a part's fields
constexpr uintptr_t kPartsBegin = 0x58;
constexpr uintptr_t kPartsEnd = 0x128;
constexpr uintptr_t kPartNodeName = 0x8;
constexpr uintptr_t kPartLimbValue = 0x100;
constexpr uintptr_t kPartType = 0x115;
constexpr uintptr_t kPartDisplayName = 0x40;
// the whole target, not a limb
constexpr uint8_t kPartTypeWhole = 0x11;
// ActorValueInfo: its type (a limb's condition is 5). 0x170 on 1.10.163 (the exe's own checks are all cmp [reg+170h],5;
// 0x1B0 is the next-gen layout)
constexpr uintptr_t kValueType = 0x170;
constexpr int32_t kValueTypeCondition = 5;

// TESObjectWEAP's animation type (or its instance's), grenades and mines (not aimed), guns
constexpr uintptr_t kWeaponType = 0x2CF;
constexpr uintptr_t kInstanceWeaponType = 0x137;
constexpr uint8_t kWeaponTypeGrenade = 10;
constexpr uint8_t kWeaponTypeMine = 11;
constexpr uint8_t kWeaponTypeGunFirst = 7;
constexpr uint8_t kWeaponTypeGunLast = 9;

// Pellets of one shot leave in the same frame
constexpr auto kSameShot = std::chrono::milliseconds(8);
constexpr float kRefreshInterval = 0.25f;
constexpr float kSettingsInterval = 1.f;
// A new target has to be this much nearer the crosshair (of the screen's size) than the one it has
constexpr float kKeepTargetMargin = 0.03f;
// How far (screen, 0..1) from the middle something can be picked, and how much nearer an enemy counts
constexpr float kMaxScreenDistance = 0.45f;
constexpr float kHostileBonus = 0.15f;
constexpr float kDefaultEngageDistance = 4096.f;
// A miss: at most this far (degrees) off the part, as the game's fVATSProjectileMissLimitDeg
constexpr float kMissLimitDegrees = 5.f;
constexpr float kMissNearestRadius = 40.f;
// The part under the crosshair can flick between two as they cross: no faster than this
constexpr auto kPartSoundGap = std::chrono::milliseconds(150);
constexpr double kDegrees = 3.14159265358979 / 180.0;

constexpr const wchar_t* kVatsUnchainedDlls[] = {L"VATSUnchainedRD.dll", L"VATSUnchained.dll"};
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"Vats76";

// What a shot needs, from the main thread to whichever fires
struct Shot
{
    bool Valid{false};
    TESObjectREFR* pTarget{nullptr};
    uint32_t Handle{0};
    glm::vec3 Point{};
    float Radius{kMissNearestRadius};
    uint32_t Limb{0xFFFFFFFF};
    float Chance{0.f};
};

Vats76Service* s_pService = nullptr;
std::atomic<bool> s_active{false};
std::atomic<bool> s_installed{false};

std::mutex s_shotLock;
Shot s_shot;
// the attack under way
uint32_t s_attack{0};
float s_attackRoll{1.f};
std::chrono::steady_clock::time_point s_lastLaunch{};
uint32_t s_lastLaunchBurst{0};
std::mt19937 s_random{std::random_device{}()};

// A burst under way: main thread and the thread firing
std::atomic<int32_t> s_burstRounds{0};
std::atomic<uint32_t> s_burstId{0};
std::atomic<float> s_burstRoundCost{0.f};
// its last round is out: the main thread ends it
std::atomic<bool> s_burstDone{false};
// the button's still down after a burst: no new attack until it's let go
std::atomic<bool> s_waitForRelease{false};
// Action Points to take, or give back, on the main thread
std::atomic<float> s_apOwed{0.f};
// an attack started this frame
std::atomic<bool> s_attackStarted{false};

// A critical strike readied by the key (main thread), taken by the next attack (the thread firing)
std::atomic<bool> s_criticalReady{false};
// the attack under way is a critical strike (the thread firing, under s_shotLock)
bool s_attackCritical{false};
// Launches since the main thread last looked: attacks that will hit (each gives the meter its charge, as the game's VATS
// does), critical strikes fired (each spends one)
std::atomic<int32_t> s_chargesOwed{0};
std::atomic<int32_t> s_strikesOwed{0};
// AimLaunch made this launch a critical strike's: OnLaunched marks its projectile (the same launch, on the same thread)
thread_local bool t_criticalLaunch = false;
// the jump press the critical key took, swallowed until it's let go
bool s_jumpTaken{false};

// A readied critical strike's gone; the kill award filled the meter
std::atomic<bool> s_fillCriticalOwed{false};

// A melee attack at the target: it wants a jump, taken on the main thread as the game moves an actor there
std::atomic<uint32_t> s_lungeWanted{0};
std::atomic<bool> s_meleeJumpOn{true};
// the last jump: how far it was, who at, when (for Blitz's damage)
std::atomic<float> s_lungeDistance{-1.f};
std::atomic<uint32_t> s_lungeTarget{0};
std::atomic<int64_t> s_lungeAt{0};
// this melee hit is a readied critical strike
thread_local bool t_forceCritical = false;

// The Mysterious Stranger. Attacks ask for a roll (any thread); the frame's update rolls, and runs his visit
struct StrangerRoll
{
    uint32_t Target{0};
    std::chrono::steady_clock::time_point Due{};
};
std::mutex s_strangerLock;
std::vector<StrangerRoll> s_strangerRolls;
// VATS 76' was turned off by the player (the button, the menu, the settings, death): his visit ends
std::atomic<bool> s_strangerCut{false};
// A visit's target and his character (handles), for the deaths' sink; what it saw
std::atomic<uint32_t> s_strangerTarget{0};
std::atomic<uint32_t> s_strangerHandle{0};
std::atomic<bool> s_strangerTargetDied{false};
std::atomic<bool> s_strangerKilled{false};

enum class StrangerStep
{
    kNone,
    // looking for a spot by the player
    kFindingSpot,
    // put in, waiting for his 3D
    kArriving,
    // on at the target
    kVisiting,
    // done: gone once his delay's up
    kLeaving,
};

// The frame's update only
struct StrangerVisit
{
    StrangerStep Step{StrangerStep::kNone};
    uint32_t Target{0};
    uint32_t Stranger{0};
    // in this step, since the last look for a spot, since he was put in (seconds)
    float Time{0.f};
    float SinceSpotTry{0.f};
    float SincePlaced{0.f};
    float LeaveAfter{0.f};
    bool Engaged{false};
    void* pImageSpace{nullptr};
};
StrangerVisit s_visit;
// The targets rolled for since it was turned on: one roll each, as the plugin does
std::vector<uint32_t> s_strangerRolled;
// His characters done with, deleted once the game's had them long enough and they've faded out
struct StrangerGone
{
    uint32_t Handle{0};
    float SincePlaced{0.f};
    float SinceGone{0.f};
};
std::vector<StrangerGone> s_strangerGone;
// it was turned on again: a new VATS 76' session, every target rolled for afresh
std::atomic<bool> s_strangerNewSession{false};
// Characters of his that aren't ours (one from a save, or one that came to us from a player who left): seconds seen
std::vector<std::pair<uint32_t, float>> s_strangerStrays;
float s_strangerSweep{0.f};

// What a perk asks about, on the thread asking
struct AttackScope
{
    TESForm* pWeapon{nullptr};
    TESObjectREFR* pTarget{nullptr};
    int32_t Limb{-1};
    uint32_t HitFlags{0};
    int32_t WeaponType{-1};
    bool Success{true};
    bool Critical{false};
};
thread_local const AttackScope* t_scope = nullptr;

class ScopeGuard
{
public:
    explicit ScopeGuard(const AttackScope* apScope) noexcept
        : m_pBefore(t_scope)
    {
        t_scope = apScope;
    }
    ~ScopeGuard() { t_scope = m_pBefore; }
    TP_NOCOPYMOVE(ScopeGuard);

private:
    const AttackScope* m_pBefore;
};

// The perks' memory of the attacks: the same part shot again (Concentrated Fire), the targets shot in a row (Gun Fu)
std::atomic<bool> s_perksOn{true};
std::mutex s_perkLock;
uint32_t s_streakTarget{0};
uint32_t s_streakLimb{0xFFFFFFFF};
int32_t s_streak{0};
std::vector<uint32_t> s_gunFuChain;
std::chrono::steady_clock::time_point s_gunFuLast{};

// The picked part's glow (main thread): our one target, the 3D and the part types it's over
struct PartGlow
{
    void* pTarget{nullptr};
    const void* p3D{nullptr};
    std::vector<uint8_t> Types;
};
PartGlow s_glow;

// A weapon instance as the game hands it out: a form and a counted pointer to its modded data
struct WeaponInstance
{
    TESForm* pForm{nullptr};
    void* pData{nullptr};

    WeaponInstance() = default;
    TP_NOCOPYMOVE(WeaponInstance);
    ~WeaponInstance()
    {
        if (!pData)
            return;
        auto* pCount = reinterpret_cast<volatile long*>(static_cast<uint8_t*>(pData) + 8);
        if (_InterlockedDecrement(pCount) == 0)
        {
            using TDelete = void(void*, int);
            (*reinterpret_cast<TDelete***>(pData))[0](pData, 1);
        }
    }
};

// A weapon instance for a game function that takes it by value. The callee owns a by-value argument and releases it as
// it returns (the attack's AP cost 85995, VATS rounds 1076921 and the hit chance 984570 all end with lock dec [data+8]),
// so each call gets its own counted copy and ours is left alone. Without it every call took a count off the equipped
// gun's modded data until the game freed it while it was still in use (crash 11-06 15:48, 1344794+0x19c).
struct WeaponArg
{
    TESForm* pForm{nullptr};
    void* pData{nullptr};

    explicit WeaponArg(const WeaponInstance& acWeapon) noexcept
        : pForm(acWeapon.pForm)
        , pData(acWeapon.pData)
    {
        if (pData)
            _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(pData) + 8));
    }
    // the callee has released it
    ~WeaponArg() = default;
    TP_NOCOPYMOVE(WeaponArg);
};
static_assert(sizeof(WeaponArg) == sizeof(WeaponInstance));

void GetPlayerWeapon(Actor* apActor, WeaponInstance& aOut) noexcept
{
    using TGet = void*(Actor*, WeaponInstance*, uint32_t);
    POINTER_FALLOUT4(TGet, s_get, kGetWeaponInstance);
    s_get.Get()(apActor, &aOut, 0);
}

[[nodiscard]] uint8_t WeaponType(const TESForm* apForm, const void* apData) noexcept
{
    if (apData)
        return *(static_cast<const uint8_t*>(apData) + kInstanceWeaponType);
    return apForm ? *(reinterpret_cast<const uint8_t*>(apForm) + kWeaponType) : 0;
}

// The whole of the player's next attack, in Action Points: the game's cost and what perks make of it
[[nodiscard]] float AttackCost(PlayerCharacter* apPlayer, WeaponInstance& aWeapon, TESObjectREFR* apTarget) noexcept
{
    if (!aWeapon.pForm)
        return 0.f;
    using TCost = float(WeaponArg*, bool, bool);
    POINTER_FALLOUT4(TCost, s_cost, kAttackActionPoints);
    WeaponArg arg(aWeapon);
    float cost = std::max(s_cost.Get()(&arg, true, false), 0.f);

    // perks ask about the weapon and the target's base form, as an instance like the weapon's
    WeaponInstance base;
    if (apTarget)
        base.pForm = *reinterpret_cast<TESForm**>(reinterpret_cast<uint8_t*>(apTarget) + kReferenceBaseForm);
    using TEntry = void(uint32_t, Actor*, ...);
    POINTER_FALLOUT4(TEntry, s_entry, kHandleEntryPoint);
    s_entry.Get()(kEntryVatsAttackCost, static_cast<Actor*>(apPlayer), &aWeapon, &base, &cost);
    return std::max(cost, 0.f);
}

[[nodiscard]] int32_t BurstRounds(WeaponInstance& aWeapon) noexcept
{
    if (!aWeapon.pForm)
        return 0;
    const uint8_t type = WeaponType(aWeapon.pForm, aWeapon.pData);
    if (type < kWeaponTypeGunFirst || type > kWeaponTypeGunLast)
        return 0;
    using TRounds = uint32_t(WeaponArg*);
    POINTER_FALLOUT4(TRounds, s_rounds, kBurstRounds);
    WeaponArg arg(aWeapon);
    return static_cast<int32_t>(s_rounds.Get()(&arg));
}

void ShowMessage(const char* apText) noexcept
{
    using TShow = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShow, s_show, kShowHudMessage);
    s_show.Get()(apText, nullptr, false, false);
}

void AddApOwed(float aAmount) noexcept
{
    float owed = s_apOwed.load();
    while (!s_apOwed.compare_exchange_weak(owed, owed + aAmount))
    {
    }
}

// A shot off the part by a random way, no further than the game's limit, in the plane across the line of fire
[[nodiscard]] glm::vec3 MissDirection(const glm::vec3& acDirection, float aRadius) noexcept
{
    const float length = glm::length(acDirection);
    if (length < 1.f)
        return acDirection;
    const glm::vec3 forward = acDirection / length;
    glm::vec3 side = glm::cross(forward, glm::vec3(0.f, 0.f, 1.f));
    if (glm::length(side) < 1e-3f)
        side = glm::vec3(1.f, 0.f, 0.f);
    side = glm::normalize(side);
    const glm::vec3 up = glm::cross(side, forward);

    std::uniform_real_distribution<float> around(0.f, 6.2831853f);
    const float angle = around(s_random);
    const float limit = std::tan(static_cast<float>(kMissLimitDegrees * kDegrees)) * length;
    const float off = std::min(aRadius * 1.5f, limit);
    return acDirection + (side * std::cos(angle) + up * std::sin(angle)) * off;
}

// The body part labels in the HUD movie, as its frame puts them there (the actor bars' way: see ActorBarsService)
struct Label
{
    std::string Text;
    // the label's place, the middle of the picked part's box (0..1 of the screen)
    float X{0.f};
    float Y{0.f};
    // the picked part's box on the screen (0..1 of its width and height)
    float Width{0.f};
    float Height{0.f};
    bool Selected{false};
};

constexpr size_t kMaxLabels = 16;
constexpr double kLabelSize = 16.;
constexpr double kSelectedSize = 22.;
// the bracket's half size (movie units): never smaller than this round a far part, never bigger than this
constexpr double kBracketMin = 10.;
constexpr double kBracketMax = 220.;
// a new size is drawn once it's this far (movie units) from the one drawn
constexpr double kBracketRedraw = 0.75;
constexpr double kTextGap = 6.;
// Labels and brackets are drawn in white and tinted to the player's HUD colour by their shader targets (colour type 2),
// as the game's own HUD is: a target multiplies its drawing by its type's colour (see HudMarker::MakeShaderTarget)
constexpr double kWhite = static_cast<double>(0xFFFFFF);
constexpr int32_t kHudColourType = 2;

class LabelsLayer final : public HudMarker::Layer
{
public:
    void SetWanted(std::vector<Label>&& aLabels) noexcept
    {
        std::scoped_lock _(m_wantedLock);
        m_wanted = std::move(aLabels);
    }

    void Advance(IMenu* apHud) noexcept override;
    void AppendShaderFXInfos(IMenu* apHud, void* apBackground, void* apForeground) noexcept override
    {
        if (!apHud || !m_pMovie || apHud->pMovie != m_pMovie)
            return;
        std::scoped_lock _(m_targetsLock);
        for (uint8_t* pTarget : m_targets)
            HudMarker::AppendShaderTarget(pTarget, apBackground, apForeground);
    }
    void TearDown(IMenu* apHud) noexcept override;

private:
    // A label: the root (put on the part) holds the text, in a sprite of its own, and the bracket. Each of those two is a
    // shader target of its own, as a target's shaded box fills its object's whole bounds: the bracket's box is its
    // rectangle round the part, the text has none
    struct Sprites
    {
        GFx::Value root;
        GFx::Value label;
        GFx::Value text;
        GFx::Value bracket;
        uint8_t* pLabelTarget{nullptr};
        uint8_t* pBracketTarget{nullptr};
        bool visible{false};
        std::string shown;
        int selected{-1};
        // the bracket's half size as drawn, and the text's place beside it
        double drawnWidth{-1.};
        double drawnHeight{-1.};
        double textX{0.};
    };

    bool Build(GFx::Movie* apMovie) noexcept;
    bool MakeSprites(GFx::Movie* apMovie, Sprites& aSprites) noexcept;
    void SetFormat(GFx::Movie* apMovie, Sprites& aSprites, bool aSelected) noexcept;
    // The four corners of a box this many units out from the middle each way (its bounds: the lines' outer edges)
    static void DrawBracket(Sprites& aSprites, double aHalfWidth, double aHalfHeight) noexcept;
    void Forget() noexcept;

    std::mutex m_wantedLock;
    std::vector<Label> m_wanted;

    GFx::Movie* m_pMovie{nullptr};
    GFx::Value m_group;
    GFx::Value m_container;
    std::array<Sprites, kMaxLabels> m_sprites;
    size_t m_made{0};

    std::mutex m_targetsLock;
    std::vector<uint8_t*> m_targets;
};

LabelsLayer* s_pLayer = nullptr;

bool LabelsLayer::Build(GFx::Movie* apMovie) noexcept
{
    m_pMovie = apMovie;
    if (!apMovie->GetVariable(&m_group, "root.CenterGroup_mc") || !m_group.IsObject())
        return false;
    apMovie->CreateObject(&m_container, "flash.display.Sprite");
    if (!m_container.IsObject())
        return false;
    m_container.SetMember("mouseEnabled", GFx::Value(false));
    m_container.SetMember("mouseChildren", GFx::Value(false));
    m_group.Invoke("addChild", nullptr, &m_container, 1);
    return true;
}

void LabelsLayer::SetFormat(GFx::Movie* apMovie, Sprites& aSprites, bool aSelected) noexcept
{
    GFx::Value format;
    apMovie->CreateObject(&format, "flash.text.TextFormat");
    if (!format.IsObject())
        return;
    format.SetMember("font", GFx::Value("$MAIN_Font_Bold"));
    format.SetMember("size", GFx::Value(aSelected ? kSelectedSize : kLabelSize));
    format.SetMember("color", GFx::Value(kWhite));
    aSprites.text.SetMember("defaultTextFormat", format);
}

bool LabelsLayer::MakeSprites(GFx::Movie* apMovie, Sprites& aSprites) noexcept
{
    apMovie->CreateObject(&aSprites.root, "flash.display.Sprite");
    apMovie->CreateObject(&aSprites.label, "flash.display.Sprite");
    apMovie->CreateObject(&aSprites.bracket, "flash.display.Sprite");
    apMovie->CreateObject(&aSprites.text, "flash.text.TextField");
    if (!aSprites.root.IsObject() || !aSprites.label.IsObject() || !aSprites.bracket.IsObject() || !aSprites.text.IsObject())
        return false;

    aSprites.text.SetMember("embedFonts", GFx::Value(true));
    aSprites.text.SetMember("selectable", GFx::Value(false));
    aSprites.text.SetMember("mouseEnabled", GFx::Value(false));
    aSprites.text.SetMember("autoSize", GFx::Value("left"));
    aSprites.label.Invoke("addChild", nullptr, &aSprites.text, 1);

    DrawBracket(aSprites, kBracketMin, kBracketMin);
    aSprites.root.Invoke("addChild", nullptr, &aSprites.bracket, 1);
    aSprites.root.Invoke("addChild", nullptr, &aSprites.label, 1);
    SetFormat(apMovie, aSprites, false);

    m_container.Invoke("addChild", nullptr, &aSprites.root, 1);
    aSprites.pBracketTarget = HudMarker::MakeShaderTarget(aSprites.bracket, kHudColourType, true);
    aSprites.pLabelTarget = HudMarker::MakeShaderTarget(aSprites.label, kHudColourType, false);
    {
        std::scoped_lock _(m_targetsLock);
        for (uint8_t* pTarget : {aSprites.pBracketTarget, aSprites.pLabelTarget})
        {
            if (pTarget)
                m_targets.push_back(pTarget);
        }
    }
    return true;
}

void LabelsLayer::DrawBracket(Sprites& aSprites, double aHalfWidth, double aHalfHeight) noexcept
{
    GFx::Value graphics;
    if (!aSprites.bracket.GetMember("graphics", &graphics) || !graphics.IsObject())
        return;
    graphics.Invoke("clear");
    const GFx::Value style[] = {GFx::Value(2.), GFx::Value(kWhite), GFx::Value(1.)};
    graphics.Invoke("lineStyle", nullptr, style, 3);
    const double w = aHalfWidth;
    const double h = aHalfHeight;
    // the corners' arms: a share of the box, so a small one stays a box of corners
    const double l = std::clamp(std::min(w, h) * 0.45, 4., 18.);
    for (const auto& corner : {std::array{-w, -h, 1., 1.}, std::array{w, -h, -1., 1.}, std::array{w, h, -1., -1.}, std::array{-w, h, 1., -1.}})
    {
        const GFx::Value from[] = {GFx::Value(corner[0] + corner[2] * l), GFx::Value(corner[1])};
        const GFx::Value at[] = {GFx::Value(corner[0]), GFx::Value(corner[1])};
        const GFx::Value to[] = {GFx::Value(corner[0]), GFx::Value(corner[1] + corner[3] * l)};
        graphics.Invoke("moveTo", nullptr, from, 2);
        graphics.Invoke("lineTo", nullptr, at, 2);
        graphics.Invoke("lineTo", nullptr, to, 2);
    }
    aSprites.drawnWidth = w;
    aSprites.drawnHeight = h;
}

void LabelsLayer::Advance(IMenu* apHud) noexcept
{
    auto* pMovie = static_cast<GFx::Movie*>(apHud->pMovie);
    if (!pMovie)
        return;

    std::vector<Label> wanted;
    {
        std::scoped_lock _(m_wantedLock);
        wanted = m_wanted;
    }

    if (pMovie != m_pMovie)
    {
        if (m_pMovie)
            Forget();
        if (wanted.empty())
            return;
        if (!Build(pMovie))
        {
            m_container = GFx::Value();
            m_group = GFx::Value();
            return;
        }
    }
    if (!m_container.IsObject())
        return;

    double left = 0.;
    double top = 0.;
    double right = 0.;
    double bottom = 0.;
    const bool onScreen = !wanted.empty() && HudMarker::ScreenToLocal(pMovie, m_container, 0.f, 0.f, left, top) && HudMarker::ScreenToLocal(pMovie, m_container, 1.f, 1.f, right, bottom);

    for (size_t i = 0; i < kMaxLabels; ++i)
    {
        Sprites& sprites = m_sprites[i];
        if (!onScreen || i >= wanted.size())
        {
            if (sprites.visible)
            {
                sprites.visible = false;
                sprites.root.SetMember("visible", GFx::Value(false));
            }
            continue;
        }
        if (i >= m_made)
        {
            if (!MakeSprites(pMovie, sprites))
                return;
            m_made = i + 1;
        }
        const Label& label = wanted[i];
        if (!sprites.visible)
        {
            sprites.visible = true;
            sprites.root.SetMember("visible", GFx::Value(true));
        }
        const int selected = label.Selected ? 1 : 0;
        if (selected != sprites.selected)
        {
            sprites.selected = selected;
            SetFormat(pMovie, sprites, label.Selected);
            sprites.bracket.SetMember("visible", GFx::Value(label.Selected));
            sprites.shown.clear();
        }
        if (sprites.shown != label.Text)
        {
            sprites.shown = label.Text;
            sprites.text.SetMember("text", GFx::Value(sprites.shown.c_str()));
            sprites.text.SetMember("y", GFx::Value(label.Selected ? -kSelectedSize * 0.7 : -kLabelSize * 0.7));
            sprites.textX = -1.;
        }
        // the picked part's bracket fits the part as it is on the screen now, its name and chance beside it
        double textX = 4.;
        if (label.Selected)
        {
            const double halfWidth = std::clamp((right - left) * label.Width * 0.5, kBracketMin, kBracketMax);
            const double halfHeight = std::clamp((bottom - top) * label.Height * 0.5, kBracketMin, kBracketMax);
            if (std::abs(halfWidth - sprites.drawnWidth) > kBracketRedraw || std::abs(halfHeight - sprites.drawnHeight) > kBracketRedraw)
                DrawBracket(sprites, halfWidth, halfHeight);
            textX = sprites.drawnWidth + kTextGap;
        }
        if (std::abs(textX - sprites.textX) > 0.5)
        {
            sprites.textX = textX;
            sprites.text.SetMember("x", GFx::Value(textX));
        }
        sprites.root.SetMember("x", GFx::Value(left + (right - left) * label.X));
        sprites.root.SetMember("y", GFx::Value(top + (bottom - top) * label.Y));
    }
}

void LabelsLayer::TearDown(IMenu* apHud) noexcept
{
    if (!apHud || !m_pMovie || apHud->pMovie != m_pMovie)
        return;
    {
        std::scoped_lock _(m_targetsLock);
        m_targets.clear();
    }
    for (size_t i = 0; i < m_made; ++i)
    {
        HudMarker::FreeShaderTarget(m_sprites[i].pBracketTarget);
        HudMarker::FreeShaderTarget(m_sprites[i].pLabelTarget);
        m_sprites[i] = Sprites{};
    }
    m_made = 0;
    if (m_group.IsObject() && m_container.IsObject())
        m_group.Invoke("removeChild", nullptr, &m_container, 1);
    m_container = GFx::Value();
    m_group = GFx::Value();
    m_pMovie = nullptr;
}

void LabelsLayer::Forget() noexcept
{
    {
        std::scoped_lock _(m_targetsLock);
        m_targets.clear();
    }
    for (size_t i = 0; i < m_made; ++i)
    {
        m_sprites[i].root.Forget();
        m_sprites[i].label.Forget();
        m_sprites[i].text.Forget();
        m_sprites[i].bracket.Forget();
        m_sprites[i] = Sprites{};
    }
    m_made = 0;
    m_container.Forget();
    m_group.Forget();
    m_pMovie = nullptr;
}

// A perk entry point: (entry, the perk owner, its filters..., float* value)
template <class... T> void PerkEntry(uint32_t aEntry, Actor* apOwner, T... aArgs) noexcept
{
    using TEntry = void(uint32_t, Actor*, ...);
    POINTER_FALLOUT4(TEntry, s_entry, kHandleEntryPoint);
    s_entry.Get()(aEntry, apOwner, aArgs...);
}

// A weapon instance that's only looked at (no count of its own): what perks take as a filter
struct WeaponView
{
    TESForm* pForm{nullptr};
    void* pData{nullptr};
};

// A new attack at this target's part: where it comes in Gun Fu's chain (0 the first target)
[[nodiscard]] int32_t NoteAttack(uint32_t aTarget, uint32_t aLimb) noexcept
{
    std::scoped_lock _(s_perkLock);
    if (s_streakTarget == aTarget && s_streakLimb == aLimb)
        ++s_streak;
    else
    {
        s_streakTarget = aTarget;
        s_streakLimb = aLimb;
        s_streak = 1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s_gunFuLast > kGunFuChain)
        s_gunFuChain.clear();
    s_gunFuLast = now;
    const auto at = std::find(s_gunFuChain.begin(), s_gunFuChain.end(), aTarget);
    if (at != s_gunFuChain.end())
        return static_cast<int32_t>(at - s_gunFuChain.begin());
    s_gunFuChain.push_back(aTarget);
    return static_cast<int32_t>(s_gunFuChain.size()) - 1;
}

// An attack at a person: a roll for the Mysterious Stranger once its hit has landed (the game brings him to a target its
// attack leaves low on health), at most one waiting per target
void QueueStrangerRoll(uint32_t aTarget) noexcept
{
    if (!aTarget)
        return;
    std::scoped_lock _(s_strangerLock);
    const auto due = std::chrono::steady_clock::now() + kStrangerRollAfter;
    for (const StrangerRoll& roll : s_strangerRolls)
    {
        if (roll.Target == aTarget)
            return;
    }
    if (s_strangerRolls.size() < kStrangerMaxRolls)
        s_strangerRolls.push_back({aTarget, due});
}

// Gun Fu's third rank: from this many targets on, an attack is critical without the meter
[[nodiscard]] bool GunFuCritical(PlayerCharacter* apPlayer, WeaponView& aWeapon, TESObjectREFR* apTarget, int32_t aChainIndex) noexcept
{
    AttackScope scope;
    scope.pWeapon = aWeapon.pForm;
    scope.pTarget = apTarget;
    const ScopeGuard guard(&scope);
    float targets = std::numeric_limits<float>::max();
    PerkEntry(kEntryGunFuTargetsForCritical, static_cast<Actor*>(apPlayer), &aWeapon, &targets);
    if (targets == std::numeric_limits<float>::max())
        return false;
    return aChainIndex >= static_cast<int32_t>(std::max(targets - 1.f, 0.f));
}

[[nodiscard]] bool IsMeleeWeapon(const TESForm* apForm, const void* apData) noexcept
{
    return !apForm || WeaponType(apForm, apData) < kWeaponTypeGunFirst;
}

// What Blitz makes of a melee attack's damage: from its bonus distance to its reach, up to its multiplier at the far end
[[nodiscard]] float BlitzDamageMult(PlayerCharacter* apPlayer, WeaponView& aWeapon, uint32_t aTarget, float aDistance) noexcept
{
    if (s_lungeTarget.load() == aTarget)
    {
        const auto since = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(s_lungeAt.load());
        if (since < kLungeLasts && s_lungeDistance.load() >= 0.f)
            aDistance = s_lungeDistance.load();
    }
    Actor* pOwner = apPlayer;
    POINTER_FALLOUT4(float, s_maxDistance, kVatsMeleeMaxDistance);
    float reach = *s_maxDistance.Get();
    PerkEntry(kEntryBlitzReach, pOwner, &aWeapon, &reach);
    reach = std::max(reach, *s_maxDistance.Get());
    float bonusFrom = reach;
    PerkEntry(kEntryBlitzBonusFrom, pOwner, &aWeapon, &bonusFrom);
    float atMax = 1.f;
    PerkEntry(kEntryBlitzDamageAtMax, pOwner, &aWeapon, &atMax);
    if (bonusFrom >= reach || atMax <= 1.f)
        return 1.f;
    const float along = std::clamp((aDistance - bonusFrom) / (reach - bonusFrom), 0.f, 1.f);
    return (atMax - 1.f) * along + 1.f;
}

// What Concentrated Fire and Gun Fu make of a ranged attack's damage
[[nodiscard]] float RangedDamageMult(PlayerCharacter* apPlayer, WeaponView& aWeapon, uint32_t aTarget) noexcept
{
    int32_t streak = 0;
    int32_t chainIndex = -1;
    {
        std::scoped_lock _(s_perkLock);
        if (s_streakTarget == aTarget)
            streak = s_streak;
        const auto at = std::find(s_gunFuChain.begin(), s_gunFuChain.end(), aTarget);
        if (at != s_gunFuChain.end())
            chainIndex = static_cast<int32_t>(at - s_gunFuChain.begin());
    }
    float mult = 1.f;
    if (streak > 1)
    {
        float value = 1.f;
        PerkEntry(kEntryConcentratedFireDamage, static_cast<Actor*>(apPlayer), &value);
        mult = value;
    }
    if (chainIndex > 0)
    {
        float value = 1.f;
        PerkEntry(kEntryGunFuDamage + static_cast<uint32_t>(std::min(chainIndex, 3)), static_cast<Actor*>(apPlayer), &aWeapon, &value);
        mult += value - 1.f;
    }
    return mult > 0.f ? mult : 1.f;
}

// GetVATSMode and GetVATSValue. Nothing changes unless a perk is being asked about one of our attacks on this thread.
using TCondition = bool(void*, void*, void*, float*);
TCondition* RealGetVatsMode = nullptr;
TCondition* RealGetVatsValue = nullptr;

bool HookGetVatsMode(void* apParams, void* apFirst, void* apSecond, float* apResult)
{
    if (!t_scope)
        return RealGetVatsMode(apParams, apFirst, apSecond, apResult);
    *apResult = 1.f;
    return true;
}

bool HookGetVatsValue(void* apParams, void* apWhat, void* apArgument, float* apResult)
{
    const AttackScope* pScope = t_scope;
    if (!pScope)
        return RealGetVatsValue(apParams, apWhat, apArgument, apResult);

    const auto inList = [apArgument](const TESForm* apForm)
    {
        using TIndex = int32_t(void*, const TESForm*);
        POINTER_FALLOUT4(TIndex, s_index, kFormListIndex);
        return apArgument && apForm && s_index.Get()(apArgument, apForm) >= 0;
    };
    const TESForm* pBase = pScope->pTarget ? *reinterpret_cast<TESForm* const*>(reinterpret_cast<const uint8_t*>(pScope->pTarget) + kReferenceBaseForm) : nullptr;

    *apResult = 0.f;
    bool yes = false;
    switch (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(apWhat)))
    {
    case 0: yes = pScope->pWeapon && apArgument == pScope->pWeapon; break;
    case 1: yes = inList(pScope->pWeapon); break;
    case 2: yes = pBase && apArgument == pBase; break;
    case 3: yes = inList(pBase); break;
    case 4:
        if (const PlayerCharacter* pPlayer = PlayerCharacter::Get(); pPlayer && pScope->pTarget)
        {
            const glm::vec3 from(pPlayer->position.x, pPlayer->position.y, pPlayer->position.z);
            const glm::vec3 to(pScope->pTarget->position.x, pScope->pTarget->position.y, pScope->pTarget->position.z);
            *apResult = glm::distance(from, to);
        }
        return true;
    case 5: yes = pScope->Limb >= 0 && static_cast<int32_t>(reinterpret_cast<intptr_t>(apArgument)) == pScope->Limb; break;
    case 7: yes = pScope->Success; break;
    case 8: yes = pScope->Critical; break;
    case 11: yes = (pScope->HitFlags >> 5) & 1; break;
    case 12: yes = (pScope->HitFlags >> 7) & 1; break;
    case 13: yes = (pScope->HitFlags >> 6) & 1; break;
    case 14: yes = (pScope->HitFlags >> 8) & 1; break;
    case 15: yes = static_cast<int32_t>(reinterpret_cast<intptr_t>(apArgument)) == pScope->WeaponType; break;
    default: break;
    }
    *apResult = yes ? 1.f : 0.f;
    return true;
}

// The perks' damage of a hit. Ours when it's the player's, while it's on, and not the game's own VATS attack
using TAttackDamage = void(uint8_t*);
TAttackDamage* s_attackDamage = nullptr;

void AttackDamage(uint8_t* apHit, bool aMelee) noexcept
{
    PlayerCharacter* pPlayer = s_perksOn.load() && s_active.load() ? PlayerCharacter::Get() : nullptr;
    const uint32_t attacker = *reinterpret_cast<const uint32_t*>(apHit + kHitAttacker);
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(apHit + kHitFlags);
    if (!pPlayer || !attacker || TESObjectREFR::GetByHandle(attacker) != pPlayer || *reinterpret_cast<void* const*>(apHit + kHitVatsCommand) || (flags & kHitNotAnAttack))
    {
        s_attackDamage(apHit);
        return;
    }

    const uint32_t victim = *reinterpret_cast<const uint32_t*>(apHit + kHitVictim);
    WeaponView weapon = *reinterpret_cast<const WeaponView*>(apHit + kHitWeapon);
    AttackScope scope;
    scope.pWeapon = weapon.pForm;
    scope.pTarget = victim ? TESObjectREFR::GetByHandle(victim) : nullptr;
    scope.HitFlags = flags;
    if (weapon.pForm && weapon.pForm->formType == FormType::Weapon)
        scope.WeaponType = WeaponType(weapon.pForm, weapon.pData);
    {
        std::scoped_lock _(s_perkLock);
        if (s_streakTarget == victim && s_streakLimb != 0xFFFFFFFF)
            scope.Limb = static_cast<int32_t>(s_streakLimb);
    }
    const ScopeGuard guard(&scope);
    s_attackDamage(apHit);

    if (!scope.pTarget)
        return;
    float mult = 1.f;
    if (aMelee)
    {
        const glm::vec3 from(pPlayer->position.x, pPlayer->position.y, pPlayer->position.z);
        const glm::vec3 to(scope.pTarget->position.x, scope.pTarget->position.y, scope.pTarget->position.z);
        mult = BlitzDamageMult(pPlayer, weapon, victim, glm::distance(from, to));
    }
    else
        mult = RangedDamageMult(pPlayer, weapon, victim);
    if (mult == 1.f)
        return;
    *reinterpret_cast<float*>(apHit + kHitBaseDamage) *= mult;
    *reinterpret_cast<float*>(apHit + kHitTotalDamage) *= mult;
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) < 20)
        spdlog::info("RD VATS 76': perks make the attack on {:X} do x{:.2f}", scope.pTarget->formID, mult);
}

// A melee hit of the player's while it's on, at the target it has: counts as an attack of the game's VATS
using TMeleeHit = void(Actor*, uint32_t, bool);
TMeleeHit* s_meleeHit = nullptr;

void HookMeleeHit(Actor* apAttacker, uint32_t aIndex, bool aHit)
{
    PlayerCharacter* pPlayer = s_active.load() ? PlayerCharacter::Get() : nullptr;
    if (!pPlayer || apAttacker != static_cast<Actor*>(pPlayer))
    {
        s_meleeHit(apAttacker, aIndex, aHit);
        return;
    }
    WeaponInstance weapon;
    GetPlayerWeapon(pPlayer, weapon);
    TESObjectREFR* pTarget = nullptr;
    uint32_t handle = 0;
    uint32_t limb = 0xFFFFFFFF;
    if (IsMeleeWeapon(weapon.pForm, weapon.pData))
    {
        std::scoped_lock _(s_shotLock);
        if (s_shot.Valid)
        {
            pTarget = s_shot.pTarget;
            handle = s_shot.Handle;
            limb = s_shot.Limb;
        }
    }
    if (!pTarget)
    {
        s_meleeHit(apAttacker, aIndex, aHit);
        return;
    }

    (void)NoteAttack(handle, limb);
    if (aHit && Cast<Actor>(pTarget))
        QueueStrangerRoll(handle);
    const bool critical = aHit && s_criticalReady.exchange(false);
    if (critical)
        s_strikesOwed.fetch_add(1);
    else if (aHit)
        s_chargesOwed.fetch_add(1);

    AttackScope scope;
    scope.pWeapon = weapon.pForm;
    scope.pTarget = pTarget;
    scope.Limb = limb == 0xFFFFFFFF ? -1 : static_cast<int32_t>(limb);
    scope.Critical = critical;
    scope.WeaponType = WeaponType(weapon.pForm, weapon.pData);
    const ScopeGuard guard(s_perksOn.load() ? &scope : t_scope);
    t_forceCritical = critical;
    s_meleeHit(apAttacker, aIndex, aHit);
    t_forceCritical = false;
    if (critical)
        spdlog::info("RD VATS 76': melee critical strike at {:X}", pTarget->formID);
}

using TCriticalChance = float(void*, void*, void*, void*);
TCriticalChance* s_criticalChance = nullptr;

float HookCriticalChance(void* apFirst, void* apSecond, void* apThird, void* apFourth)
{
    const float chance = s_criticalChance(apFirst, apSecond, apThird, apFourth);
    return t_forceCritical ? kCertainCritical : chance;
}

// The jump: on the main thread, at a target that can still be reached, the way the game's VATS moves a melee attacker
void TryLunge(uint32_t aTarget) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    Actor* pTarget = Cast<Actor>(TESObjectREFR::GetByHandle(aTarget));
    if (!pPlayer || !pTarget || pPlayer->IsDead() || pTarget->IsDead() || !pTarget->GetNiNode())
        return;
    if ((*(reinterpret_cast<const uint8_t*>(pPlayer) + kPlayerWeaponState) & kWeaponStateMask) < kWeaponStateDrawn)
        return;
    WeaponInstance weapon;
    GetPlayerWeapon(pPlayer, weapon);
    if (!IsMeleeWeapon(weapon.pForm, weapon.pData))
        return;

    const glm::vec3 from(pPlayer->position.x, pPlayer->position.y, pPlayer->position.z);
    const glm::vec3 to(pTarget->position.x, pTarget->position.y, pTarget->position.z);
    const float distance = glm::distance(from, to);
    using TReach = float(Actor*, WeaponInstance*);
    POINTER_FALLOUT4(TReach, s_reach, kGetMeleeReach);
    const float reach = s_reach.Get()(pPlayer, &weapon);
    // already in reach (no jump), or a weapon with none
    if (reach <= 0.f || distance <= reach)
        return;

    // the game's chance is 0 past fVATSMeleeMaxDistance and what Blitz adds
    AttackScope scope;
    scope.pWeapon = weapon.pForm;
    scope.pTarget = pTarget;
    scope.WeaponType = WeaponType(weapon.pForm, weapon.pData);
    float chance = 0.f;
    {
        const ScopeGuard guard(&scope);
        using TChance = float(TESObjectREFR*, WeaponArg*, const void*, float);
        POINTER_FALLOUT4(TChance, s_chance, kHitChance);
        WeaponArg arg(weapon);
        chance = s_chance.Get()(pTarget, &arg, nullptr, 1.f);
    }
    if (chance <= 0.f)
    {
        spdlog::info("RD VATS 76': no melee jump at {:X}, out of range ({:.0f} units)", pTarget->formID, distance);
        return;
    }

    using TFindSpot = bool(void*, Actor*, Actor*, float, float*);
    POINTER_FALLOUT4(TFindSpot, s_findSpot, kFindMeleeSpot);
    POINTER_FALLOUT4(void*, s_finder, kMeleeSpotFinder);
    float spot[4]{};
    void* pFinder = *s_finder.Get();
    if (!pFinder || !s_findSpot.Get()(pFinder, pPlayer, pTarget, reach * kLungeLandAt, spot))
    {
        spdlog::info("RD VATS 76': no melee jump at {:X}, no safe path ({:.0f} units)", pTarget->formID, distance);
        return;
    }

    s_lungeDistance.store(distance);
    s_lungeTarget.store(aTarget);
    s_lungeAt.store(std::chrono::steady_clock::now().time_since_epoch().count());

    using TTeleport = void(Actor**, Actor**, float);
    POINTER_FALLOUT4(TTeleport, s_teleport, kMeleeTeleport);
    Actor* pAttacker = pPlayer;
    Actor* pVictim = pTarget;
    s_teleport.Get()(&pAttacker, &pVictim, reach * kLungeLandAt);
    spdlog::info("RD VATS 76': melee jump at {:X}, {:.0f} units (reach {:.0f})", pTarget->formID, distance, reach);
}

void HookAttackDamageMelee(uint8_t* apHit) { AttackDamage(apHit, true); }
void HookAttackDamageRanged(uint8_t* apHit) { AttackDamage(apHit, false); }

// An explosion set up: one the player shot gets Demolition Expert's damage, as in the game's VATS
using TExplosionSetup = uint64_t(uint8_t*, uint8_t*, void*, void*);
TExplosionSetup* RealExplosionSetup = nullptr;

uint64_t HookExplosionSetup(uint8_t* apExplosion, uint8_t* apData, void* apThird, void* apFourth)
{
    const uint64_t result = RealExplosionSetup(apExplosion, apData, apThird, apFourth);
    if (!s_perksOn.load() || !s_active.load() || !apExplosion || !apData)
        return result;
    POINTER_FALLOUT4(const uint8_t*, s_vats, kVatsSingleton);
    if (const uint8_t* pVats = *s_vats.Get(); pVats && *reinterpret_cast<const int32_t*>(pVats + kVatsPlaying) != 0)
        return result;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    auto* pSource = *reinterpret_cast<TESObjectREFR**>(apData + kExplosionSource);
    if (!pPlayer || !pSource)
        return result;
    if (*reinterpret_cast<void**>(apData + kExplosionOwner) != pPlayer && pSource != static_cast<TESObjectREFR*>(pPlayer))
        return result;
    using TAsProjectile = uint8_t*(TESObjectREFR*);
    const uint8_t* pProjectile = (*reinterpret_cast<TAsProjectile* const* const*>(pSource))[kAsProjectile](pSource);
    if (!pProjectile || !(*reinterpret_cast<const uint64_t*>(pProjectile + kProjectileFlags) & kProjectileShotExplosive))
        return result;
    const AttackScope scope;
    const ScopeGuard guard(&scope);
    auto* pDamage = reinterpret_cast<float*>(apExplosion + kExplosionDamage);
    const float before = *pDamage;
    PerkEntry(kEntryShootExplosiveDamage, static_cast<Actor*>(pPlayer), pDamage);
    if (*pDamage != before)
        spdlog::info("RD VATS 76': shot explosive's damage {:.1f} -> {:.1f}", before, *pDamage);
    return result;
}

// A kill while it's on: the perks' chance of the Action Points back (Grim Reaper's Sprint), and of a full critical meter too
struct TESDeathEvent
{
    TESObjectREFR* pDying;
    TESObjectREFR* pKiller;
    bool Dead;
};

class DeathSink final : public BSTEventSink<TESDeathEvent>
{
public:
    BSTEventResult OnEvent(const TESDeathEvent* apEvent, const EventDispatcher<TESDeathEvent>*) override;
};
DeathSink s_deathSink;

BSTEventResult DeathSink::OnEvent(const TESDeathEvent* apEvent, const EventDispatcher<TESDeathEvent>*)
{
    // (it comes twice: dying, then dead)
    if (!apEvent || apEvent->Dead)
        return BSTEventResult::kOk;
    // the Mysterious Stranger's target: his visit's over, and was it his kill
    if (const uint32_t target = s_strangerTarget.load(); target && apEvent->pDying && apEvent->pDying->GetHandle().handle.iBits == target)
    {
        const uint32_t stranger = s_strangerHandle.load();
        s_strangerKilled.store(stranger && apEvent->pKiller && apEvent->pKiller->GetHandle().handle.iBits == stranger);
        s_strangerTargetDied.store(true);
    }
    if (!s_perksOn.load() || !s_active.load())
        return BSTEventResult::kOk;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || apEvent->pKiller != static_cast<TESObjectREFR*>(pPlayer) || apEvent->pDying == static_cast<TESObjectREFR*>(pPlayer))
        return BSTEventResult::kOk;

    WeaponInstance weapon;
    GetPlayerWeapon(pPlayer, weapon);
    WeaponView view{weapon.pForm, weapon.pData};
    AttackScope scope;
    scope.pWeapon = weapon.pForm;
    scope.pTarget = apEvent->pDying;
    const ScopeGuard guard(&scope);

    Actor* pOwner = pPlayer;
    float chance = 0.f;
    PerkEntry(kEntryApKillChance, pOwner, &view, &pOwner, &chance);
    if (chance <= 0.f)
        return BSTEventResult::kOk;
    float roll = 1.f;
    {
        std::scoped_lock _(s_shotLock);
        roll = std::uniform_real_distribution<float>(0.f, 1.f)(s_random);
    }
    if (roll > chance)
        return BSTEventResult::kOk;

    const float missing = pPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints) - pPlayer->GetActorValue(ActorValueInfo::kActionPoints);
    if (missing > 0.f)
        AddApOwed(-missing);
    float fill = 0.f;
    PerkEntry(kEntryFillCriticalOnApReward, pOwner, &fill);
    if (fill > 0.f)
        s_fillCriticalOwed.store(true);
    spdlog::info("RD VATS 76': a kill gives the Action Points back (chance {:.2f}, {:.1f} AP){}", chance, missing, fill > 0.f ? ", and fills the critical meter" : "");
    return BSTEventResult::kOk;
}

// --- The hooks ---

using TVatsButton = uintptr_t(void*, void*, void*, void*);
using TAttackStart = uintptr_t(void*, void*, void*, void*);
using TAttackRelease = bool(void*, int32_t, uint32_t);
TVatsButton* s_openVats = nullptr;
TAttackStart* s_attackStart = nullptr;
TAttackRelease* s_attackRelease = nullptr;

// The VATS button's handler goes on to open the VATS menu here. (A ButtonEvent: how far it's down, how long it has been.)
constexpr uintptr_t kButtonValue = 0x38;
constexpr uintptr_t kButtonHeldTime = 0x3C;

uintptr_t HookOpenVats(void* apEvent, void* apB, void* apC, void* apD) noexcept
{
    // switched off in the settings, or no game yet: the game's own VATS
    if (!Vats76Service::TakesVatsButton())
        return s_openVats(apEvent, apB, apC, apD);
    const auto* pEvent = static_cast<const uint8_t*>(apEvent);
    if (pEvent && *reinterpret_cast<const float*>(pEvent + kButtonValue) != 0.f && *reinterpret_cast<const float*>(pEvent + kButtonHeldTime) == 0.f)
        Vats76Service::OnVatsButton();
    return 0;
}

uintptr_t HookAttackStart(void* apA, void* apB, void* apC, void* apD) noexcept
{
    if (Vats76Service::SwallowsAttackStart())
        return 0;
    const uintptr_t result = s_attackStart(apA, apB, apC, apD);
    Vats76Service::OnAttackStarted();
    return result;
}

bool HookAttackRelease(void* apHandler, int32_t aAction, uint32_t aC) noexcept
{
    if (aAction == kAttackAction)
    {
        // the button's up: the next press may start an attack, whatever else happens
        s_waitForRelease.store(false);
        if (Vats76Service::SwallowsAttackRelease())
            return true;
    }
    return s_attackRelease(apHandler, aAction, aC);
}

[[nodiscard]] bool IsVatsUnchainedInstalled() noexcept
{
    // the game's folder, as the script extender is found (ScriptExtender.cpp)
    const auto plugins = std::filesystem::current_path() / L"Data" / L"F4SE" / L"Plugins";
    for (const wchar_t* pDll : kVatsUnchainedDlls)
    {
        std::error_code ec;
        if (std::filesystem::exists(plugins / pDll, ec))
            return true;
    }
    return false;
}

// A call in a game function, if it still calls what it should: nothing is touched otherwise
[[nodiscard]] uint8_t* CheckSite(uint32_t aFunction, uint32_t aOffset, uint32_t aCallee, void* apHook) noexcept
{
    const VersionDbPtr<uint8_t> function(aFunction);
    const VersionDbPtr<uint8_t> callee(aCallee);
    uint8_t* pSite = function.Get() ? function.Get() + aOffset : nullptr;
    if (!pSite || !callee.Get() || pSite[0] != 0xE8)
        return nullptr;
    if (pSite + 5 + *reinterpret_cast<const int32_t*>(pSite + 1) != callee.Get())
        return nullptr;
    const intptr_t reach = reinterpret_cast<intptr_t>(apHook) - reinterpret_cast<intptr_t>(pSite + 5);
    return reach == static_cast<int32_t>(reach) ? pSite : nullptr;
}

TiltedPhoques::Initializer s_vats76Hooks(
    []()
    {
        if (IsVatsUnchainedInstalled())
        {
            spdlog::info("RD VATS 76': off, VATS Unchained is installed and does the same (remove it to use ours)");
            return;
        }

        uint8_t* pButton = CheckSite(kVatsButtonHandler, kVatsButtonSite, kOpenVats, reinterpret_cast<void*>(&HookOpenVats));
        uint8_t* pPress = CheckSite(kAttackHandler, kAttackHandlerStartSite, kAttackStart, reinterpret_cast<void*>(&HookAttackStart));
        uint8_t* pRearm = CheckSite(kAttackRearm, kAttackRearmStartSite, kAttackStart, reinterpret_cast<void*>(&HookAttackStart));
        uint8_t* pMeleeHit = CheckSite(kMessageDispatcher, kMeleeHitSite, kMeleeHit, reinterpret_cast<void*>(&HookMeleeHit));
        uint8_t* pCriticalChance = CheckSite(kCriticalDamage, kCriticalChanceSite, kCriticalChance, reinterpret_cast<void*>(&HookCriticalChance));
        uint8_t* pMeleeDamage = CheckSite(kMeleeHitSetup, kMeleeHitDamageSite, kAttackDamage, reinterpret_cast<void*>(&HookAttackDamageMelee));
        uint8_t* pProjectileDamage = CheckSite(kProjectileHitSetup, kProjectileHitDamageSite, kAttackDamage, reinterpret_cast<void*>(&HookAttackDamageRanged));
        uint8_t* pExplosionDamage = CheckSite(kExplosionHitSetup, kExplosionHitDamageSite, kAttackDamage, reinterpret_cast<void*>(&HookAttackDamageRanged));
        uint8_t* pRelease[std::size(kAttackHandlerReleaseSites)]{};
        bool releases = true;
        for (size_t i = 0; i < std::size(kAttackHandlerReleaseSites); ++i)
        {
            pRelease[i] = CheckSite(kAttackHandler, kAttackHandlerReleaseSites[i], kAttackRelease, reinterpret_cast<void*>(&HookAttackRelease));
            releases = releases && pRelease[i];
        }
        if (!pButton || !pPress || !pRearm || !releases)
        {
            spdlog::warn("RD VATS 76': off, the game's VATS button or attack calls aren't the expected ones (another mod may have them)");
            return;
        }

        const VersionDbPtr<uint8_t> openVats(kOpenVats);
        const VersionDbPtr<uint8_t> attackStart(kAttackStart);
        const VersionDbPtr<uint8_t> attackRelease(kAttackRelease);
        s_openVats = reinterpret_cast<TVatsButton*>(openVats.Get());
        s_attackStart = reinterpret_cast<TAttackStart*>(attackStart.Get());
        s_attackRelease = reinterpret_cast<TAttackRelease*>(attackRelease.Get());

        TiltedPhoques::PutCall(pButton, &HookOpenVats);
        TiltedPhoques::PutCall(pPress, &HookAttackStart);
        TiltedPhoques::PutCall(pRearm, &HookAttackStart);
        for (uint8_t* pSite : pRelease)
            TiltedPhoques::PutCall(pSite, &HookAttackRelease);

        // The perks: without their calls the rest of it still works, the perks' damage stays the game's
        if (pMeleeDamage && pProjectileDamage && pExplosionDamage)
        {
            const VersionDbPtr<uint8_t> attackDamage(kAttackDamage);
            s_attackDamage = reinterpret_cast<TAttackDamage*>(attackDamage.Get());
            TiltedPhoques::PutCall(pMeleeDamage, &HookAttackDamageMelee);
            TiltedPhoques::PutCall(pProjectileDamage, &HookAttackDamageRanged);
            TiltedPhoques::PutCall(pExplosionDamage, &HookAttackDamageRanged);
        }
        else
            spdlog::warn("RD VATS 76': the perks' damage stays the game's, the calls aren't the expected ones (another mod may have them)");
        // A melee attack's critical strike
        if (pMeleeHit && pCriticalChance)
        {
            const VersionDbPtr<uint8_t> meleeHit(kMeleeHit);
            const VersionDbPtr<uint8_t> criticalChance(kCriticalChance);
            s_meleeHit = reinterpret_cast<TMeleeHit*>(meleeHit.Get());
            s_criticalChance = reinterpret_cast<TCriticalChance*>(criticalChance.Get());
            TiltedPhoques::PutCall(pMeleeHit, &HookMeleeHit);
            TiltedPhoques::PutCall(pCriticalChance, &HookCriticalChance);
        }
        else
            spdlog::warn("RD VATS 76': melee attacks stay the game's, the calls aren't the expected ones (another mod may have them)");
        POINTER_FALLOUT4(TCondition, s_getVatsMode, kGetVatsMode);
        POINTER_FALLOUT4(TCondition, s_getVatsValue, kGetVatsValue);
        POINTER_FALLOUT4(TExplosionSetup, s_explosionSetup, kExplosionSetup);
        RealGetVatsMode = s_getVatsMode.Get();
        RealGetVatsValue = s_getVatsValue.Get();
        RealExplosionSetup = s_explosionSetup.Get();
        TP_HOOK(&RealGetVatsMode, HookGetVatsMode);
        TP_HOOK(&RealGetVatsValue, HookGetVatsValue);
        TP_HOOK(&RealExplosionSetup, HookExplosionSetup);
        s_installed.store(true);
        spdlog::info("RD VATS 76': on");
    });

// InventoryUserUIUtils::PlayMenuSound: a sound form by its editor id, as the VATS menu plays its own
void PlaySound(bool aSounds, const char* apSound) noexcept
{
    if (!aSounds)
        return;
    using TPlay = void(const char*);
    POINTER_FALLOUT4(TPlay, s_play, 1227994);
    s_play.Get()(apSound);
}

// The meters the game's VATS keeps up: up while it's on, as the VATS menu has them
void ForceShowMeters(bool aShow) noexcept
{
    using TForce = void(uint32_t, bool);
    POINTER_FALLOUT4(TForce, s_force, kForceShowMeter);
    for (const uint32_t meter : kVatsMeters)
        s_force.Get()(meter, aShow);
}

// A critical strike is there to use: the charge is full, or one is banked
[[nodiscard]] bool CriticalFull(PlayerCharacter* apPlayer) noexcept
{
    if (!apPlayer)
        return false;
    POINTER_FALLOUT4(float, s_limit, kCriticalChargeLimit);
    const auto* pPlayer = reinterpret_cast<const uint8_t*>(apPlayer);
    const float charge = *reinterpret_cast<const float*>(pPlayer + kCriticalCharge);
    const uint32_t banked = *reinterpret_cast<const uint32_t*>(pPlayer + kCriticalBanked);
    return banked > 0 || charge >= *s_limit.Get();
}

// --- the picked part's glow

volatile long* GlowRefCount(void* apTarget) noexcept
{
    return static_cast<volatile long*>(apTarget);
}

void ReleaseGlowTarget(void*& apTarget) noexcept
{
    if (!apTarget)
        return;
    if (_InterlockedDecrement(GlowRefCount(apTarget)) == 0)
    {
        using TDestructor = void(void*);
        POINTER_FALLOUT4(TDestructor, s_destructor, kGlowDestructor);
        s_destructor.Get()(apTarget);
        Memory::Free(apTarget);
    }
    apTarget = nullptr;
}

// Ours is still among the manager's targets (VATS and the workshop clear them all when they start)
[[nodiscard]] bool GlowStillUp() noexcept
{
    return s_glow.pTarget && *GlowRefCount(s_glow.pTarget) > 1;
}

void ClearGlow() noexcept
{
    if (GlowStillUp())
    {
        using TRemove = void(void**);
        using TDisable = void(bool);
        POINTER_FALLOUT4(TRemove, s_remove, kGlowRemoveTarget);
        POINTER_FALLOUT4(TDisable, s_disable, kGlowDisable);
        POINTER_FALLOUT4(uint32_t, s_count, kGlowCount);
        s_remove.Get()(&s_glow.pTarget);
        // the effect goes off with the last target (anyone else's keeps it on)
        if (*s_count.Get() == 0)
            s_disable.Get()(false);
    }
    ReleaseGlowTarget(s_glow.pTarget);
    s_glow = PartGlow{};
}

// The glow over these part types of a 3D, in the HUD colour (a 3D or types not the ones it has: made again)
void SetGlow(const void* ap3D, const std::vector<uint8_t>& acTypes) noexcept
{
    if (ap3D && s_glow.p3D == ap3D && s_glow.Types == acTypes && GlowStillUp())
        return;
    ClearGlow();
    if (!ap3D || acTypes.empty())
        return;

    using TConstructor = void*(void*, const void*);
    using TAddSegment = void(void*, uint8_t, uint8_t);
    using TAddTarget = void(void**);
    using TEnable = void(bool, uint8_t);
    using TColour = void(float, float, float);
    POINTER_FALLOUT4(TConstructor, s_constructor, kGlowConstructor);
    POINTER_FALLOUT4(TAddSegment, s_addSegment, kGlowAddSegment);
    POINTER_FALLOUT4(TAddTarget, s_addTarget, kGlowAddTarget);
    POINTER_FALLOUT4(TEnable, s_enable, kGlowEnable);
    POINTER_FALLOUT4(TColour, s_colour, kGlowColour);
    POINTER_FALLOUT4(float, s_hudColour, kHudColour);

    void* pMemory = Memory::Allocate(kGlowSize);
    if (!pMemory)
        return;
    void* pTarget = s_constructor.Get()(pMemory, ap3D);
    _InterlockedIncrement(GlowRefCount(pTarget));
    // nothing to draw in it (no geometry): the game doesn't take it
    if (*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pTarget) + kGlowGeometryCount) == 0)
    {
        ReleaseGlowTarget(pTarget);
        return;
    }
    for (const uint8_t type : acTypes)
        s_addSegment.Get()(pTarget, type, kGlowAllSubsegments);
    s_addTarget.Get()(&pTarget);
    s_enable.Get()(false, 0);
    const float* pColour = s_hudColour.Get();
    s_colour.Get()(pColour[0], pColour[1], pColour[2]);
    s_glow = {pTarget, ap3D, acTypes};
}

const void* Get3D(TESObjectREFR* apRef) noexcept
{
    using TGet3D = void*(TESObjectREFR*);
    return (*reinterpret_cast<TGet3D* const* const*>(apRef))[kGet3D](apRef);
}

// The live mines and thrown grenades, as handles. The list's lock is only tried (a frame's update shouldn't wait on the
// thread moving projectiles), nothing this time when it's taken.
void ListExplosives(std::vector<uint32_t>& aOut) noexcept
{
    aOut.clear();
    POINTER_FALLOUT4(uint8_t*, s_manager, kProjectileManager);
    uint8_t* pManager = *s_manager.Get();
    if (!pManager)
        return;
    auto* pOwner = reinterpret_cast<volatile long*>(pManager + kProjectileLockOwner);
    auto* pCount = reinterpret_cast<volatile long*>(pManager + kProjectileLockCount);
    const long thread = static_cast<long>(GetCurrentThreadId());
    if (*pOwner == thread)
        _InterlockedIncrement(pCount);
    else
    {
        if (_InterlockedCompareExchange(pCount, 1, 0) != 0)
            return;
        *pOwner = thread;
    }

    using TIsGrenade = bool(TESObjectREFR*);
    for (const uintptr_t list : kProjectileLists)
    {
        auto* const* ppData = *reinterpret_cast<TESObjectREFR* const* const*>(pManager + list + kListData);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(pManager + list + kListSize);
        if (!ppData)
            continue;
        for (uint32_t i = 0; i < size; ++i)
        {
            TESObjectREFR* pProjectile = ppData[i];
            if (pProjectile && (*reinterpret_cast<TIsGrenade* const* const*>(pProjectile))[kIsGrenadeProjectile](pProjectile))
                aOut.push_back(pProjectile->GetHandle().handle.iBits);
        }
    }

    if (*pCount == 1)
    {
        *pOwner = 0;
        _InterlockedExchange(pCount, 0);
    }
    else
        _InterlockedDecrement(pCount);
}

// Where a thing is: the middle of its 3D's bound, else its position
[[nodiscard]] glm::vec3 BoundCentre(TESObjectREFR* apRef) noexcept
{
    if (const auto* p3D = static_cast<const uint8_t*>(Get3D(apRef)); p3D && *reinterpret_cast<const float*>(p3D + kWorldBoundRadius) > 0.f)
    {
        const auto* pCentre = reinterpret_cast<const float*>(p3D + kWorldBoundCentre);
        return {pCentre[0], pCentre[1], pCentre[2]};
    }
    return {apRef->position.x, apRef->position.y, apRef->position.z};
}

// --- MCM's keybinds

// A number field of a flat JSON object, or aDefault
[[nodiscard]] int JsonInt(std::string_view aObject, std::string_view aKey, int aDefault) noexcept
{
    const std::string pattern = MiniJson::Quote(aKey);
    size_t pos = aObject.find(pattern);
    if (pos == std::string_view::npos)
        return aDefault;
    pos = aObject.find(':', pos + pattern.size());
    if (pos == std::string_view::npos)
        return aDefault;
    ++pos;
    while (pos < aObject.size() && std::isspace(static_cast<unsigned char>(aObject[pos])))
        ++pos;
    int value = 0;
    const auto [pEnd, error] = std::from_chars(aObject.data() + pos, aObject.data() + aObject.size(), value);
    return error == std::errc{} && pEnd != aObject.data() + pos ? value : aDefault;
}

// Our key in MCM's Keybinds.json ({"keybinds":[{"id","keycode","modName","modifiers"}, ...]}): false when it has none
[[nodiscard]] bool FindKeybind(const std::string& acJson, int& aKey, uint32_t& aModifiers) noexcept
{
    size_t pos = 0;
    while ((pos = acJson.find('{', pos + 1)) != std::string::npos)
    {
        const size_t end = acJson.find('}', pos);
        if (end == std::string::npos)
            break;
        const std::string_view object(acJson.data() + pos, end - pos + 1);
        if (MiniJson::GetString(object, "modName") == kModName && MiniJson::GetString(object, "id") == kCriticalKeyId)
        {
            aKey = JsonInt(object, "keycode", 0);
            aModifiers = static_cast<uint32_t>(std::max(JsonInt(object, "modifiers", 0), 0));
            return aKey > 0;
        }
        pos = end;
    }
    return false;
}

// The game's window is in front (the keys aren't going to another window)
[[nodiscard]] bool GameHasKeyboard() noexcept
{
    HWND window = GetForegroundWindow();
    DWORD processId = 0;
    if (window)
        GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

[[nodiscard]] bool ModifiersDown(uint32_t aModifiers) noexcept
{
    const auto down = [](int aKey) { return (GetAsyncKeyState(aKey) & 0x8000) != 0; };
    return (!(aModifiers & 1) || down(VK_SHIFT)) && (!(aModifiers & 2) || down(VK_CONTROL)) && (!(aModifiers & 4) || down(VK_MENU));
}

// --- the Mysterious Stranger

// A VATS default object's form (of that form type when one's given)
[[nodiscard]] TESForm* DefaultObjectForm(uint32_t aObject, int aType = -1) noexcept
{
    const VersionDbPtr<uint8_t> object(aObject);
    if (!object.Get())
        return nullptr;
    TESForm* pForm = *reinterpret_cast<TESForm* const*>(object.Get() + kDefaultObjectForm);
    if (!pForm || (aType >= 0 && static_cast<uint8_t>(pForm->formType) != aType))
        return nullptr;
    return pForm;
}

// Where he stands, as the plugin looks: beside or behind the player, on ground near the player's height, the line clear to
// the player and to the target (the target itself in the way is fine)
[[nodiscard]] bool FindStrangerSpot(PlayerCharacter* apPlayer, Actor* apTarget, NiPoint3& aSpot) noexcept
{
    TESObjectCELL* pCell = apPlayer->GetParentCellEx();
    const auto* pTarget3D = static_cast<const uint8_t*>(Get3D(apTarget));
    if (!pCell || !pTarget3D)
        return false;
    const uint32_t filter = WorldRays::GetLineOfSightFilter(apPlayer);
    const glm::vec3 player(apPlayer->position.x, apPlayer->position.y, apPlayer->position.z);
    const auto* pCentre = reinterpret_cast<const float*>(pTarget3D + kWorldBoundCentre);
    const glm::vec3 centre(pCentre[0], pCentre[1], pCentre[2]);
    const float radius = *reinterpret_cast<const float*>(pTarget3D + kWorldBoundRadius);
    glm::vec3 forward(apTarget->position.x - player.x, apTarget->position.y - player.y, 0.f);
    const float length = glm::length(forward);
    forward = length > 1.f ? forward / length : glm::vec3(0.f);
    const glm::vec3 side(forward.y, -forward.x, 0.f);
    const glm::vec3 up(0.f, 0.f, 1.f);
    const glm::vec3 playerEye = player + up * kStrangerEyeHeight;

    const auto clear = [&](const glm::vec3& acFrom, const glm::vec3& acTo, bool aTargetCounts)
    {
        const WorldRays::RayHit hit = WorldRays::PickOnce(pCell, filter, NiPoint3(acFrom), NiPoint3(acTo));
        if (!hit.Hit)
            return true;
        return aTargetCounts && WorldRays::IsCharacterLayer(hit.Layer) && glm::distance(glm::vec3(hit.Point), centre) <= radius + kStrangerRayGap;
    };

    for (const auto& spot : kStrangerSpots)
    {
        const glm::vec3 at = player + side * spot[0] - forward * spot[1];
        const WorldRays::RayHit ground = WorldRays::PickOnce(pCell, filter, NiPoint3(at + up * kStrangerGroundReach), NiPoint3(at - up * kStrangerGroundReach));
        if (!ground.Hit || WorldRays::IsCharacterLayer(ground.Layer))
            continue;
        const glm::vec3 feet(ground.Point);
        if (std::abs(feet.z - player.z) > kStrangerMaxStep)
            continue;
        const glm::vec3 eye = feet + up * kStrangerEyeHeight;
        const glm::vec3 toPlayer = eye - playerEye;
        const float apart = glm::length(toPlayer);
        if (apart <= kStrangerMaxStep)
            continue;
        const glm::vec3 alongPlayer = toPlayer / apart;
        if (!clear(playerEye + alongPlayer * kStrangerRayGap, eye - alongPlayer * kStrangerRayGap, false))
            continue;
        const glm::vec3 toTarget = centre - eye;
        const float reach = glm::length(toTarget);
        if (reach < kStrangerRayGap * 2.f || !clear(eye + toTarget / reach * kStrangerRayGap, centre, true))
            continue;
        aSpot = NiPoint3(feet);
        spdlog::info("RD VATS 76': Mysterious Stranger: spot {:.0f} side / {:.0f} back", spot[0], spot[1]);
        return true;
    }
    return false;
}

// A character of his own at the spot, put in the way a script places one (ObjectReference.PlaceActorAtMe, as the nemeses
// are), his AI reset as the game's VATS does before it moves him in
[[nodiscard]] Actor* PutStrangerAt(const NiPoint3& acSpot) noexcept
{
    auto* pNpc = Cast<TESNPC>(TESForm::GetById(kStrangerNpc));
    if (!pNpc)
        return nullptr;
    Actor* pStranger = NemesisGame::Spawn(pNpc, acSpot);
    if (!pStranger)
        return nullptr;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pStranger) + kActorFlags) |= kActorResetAI;
    return pStranger;
}

// On at the target, as 430566 has him: turned to it, it his linked ref for his package, then the image space, the sting,
// the perk's Vault Boy clip and the rank 3 entry point that fills the critical meter
void EngageStranger(PlayerCharacter* apPlayer, Actor* apStranger, Actor* apTarget, StrangerVisit& aVisit) noexcept
{
    using TFace = void(Actor*, TESObjectREFR*);
    using TUpdate = void(Actor*);
    using TEvaluate = void(Actor*, bool, bool);
    POINTER_FALLOUT4(TFace, s_face, kFaceTarget);
    POINTER_FALLOUT4(TUpdate, s_update, kUpdateActor);
    POINTER_FALLOUT4(TEvaluate, s_evaluate, kEvaluatePackage);
    s_face.Get()(apStranger, apTarget);
    s_update.Get()(apStranger);
    apStranger->SetLinkedRef(apTarget, nullptr);
    s_evaluate.Get()(apStranger, false, false);

    if (TESForm* pImageSpace = DefaultObjectForm(kStrangerImageSpaceObject))
    {
        using TApply = void*(TESForm*, float);
        POINTER_FALLOUT4(TApply, s_apply, kApplyImageSpace);
        s_apply.Get()(pImageSpace, kStrangerImageSpaceStrength);
        aVisit.pImageSpace = pImageSpace;
    }
    PlaySound(true, kStrangerMusicIn);
    using TSoundForm = void*(const char*);
    using TClip = void(const BSFixedString*, void*);
    POINTER_FALLOUT4(TSoundForm, s_soundForm, kSoundFormByName);
    POINTER_FALLOUT4(TClip, s_clip, kShowPerkClip);
    void* pSound = s_soundForm.Get()(kStrangerClipSound);
    const BSFixedString clip(kStrangerClip);
    s_clip.Get()(&clip, pSound);

    float fill = 0.f;
    PerkEntry(kEntryStrangerFillsCritical, static_cast<Actor*>(apPlayer), &fill);
    if (fill > 0.f)
        s_fillCriticalOwed.store(true);
    aVisit.Engaged = true;
    spdlog::info("RD VATS 76': Mysterious Stranger: engaging the target {:X}{}", apTarget->formID, fill > 0.f ? ", and he fills the critical meter" : "");
}

// What's left of his visit is let go of; his character (if he came) goes as the game's VATS sends him off: no linked ref,
// the note that he came, out (with the fade), the image space off. He's deleted once the game's had him long enough
void EndStrangerVisit(StrangerVisit& aVisit, const char* apWhy) noexcept
{
    if (Actor* pStranger = aVisit.Stranger ? Cast<Actor>(TESObjectREFR::GetByHandle(aVisit.Stranger)) : nullptr)
    {
        pStranger->SetLinkedRef(nullptr, nullptr);
        pStranger->Disable();
        s_strangerGone.push_back({aVisit.Stranger, aVisit.SincePlaced, 0.f});
    }
    if (aVisit.Engaged)
    {
        using TVisited = void();
        POINTER_FALLOUT4(TVisited, s_visited, kStrangerVisited);
        s_visited.Get()();
    }
    if (aVisit.pImageSpace)
    {
        using TRemove = void(void*);
        POINTER_FALLOUT4(TRemove, s_remove, kRemoveImageSpace);
        s_remove.Get()(aVisit.pImageSpace);
    }
    spdlog::info("RD VATS 76': Mysterious Stranger: gone ({})", apWhy);
    s_strangerTarget.store(0);
    s_strangerHandle.store(0);
    s_strangerTargetDied.store(false);
    s_strangerKilled.store(false);
    aVisit = StrangerVisit{};
}

// His characters done with are deleted once the game has had them a while and they've faded out; any other character of
// his here that's this machine's (left in a save, or handed to us when the player he came for left) goes the same way
void SweepStrangers(float aDelta, bool aBusy) noexcept
{
    for (auto it = s_strangerGone.begin(); it != s_strangerGone.end();)
    {
        it->SincePlaced += aDelta;
        it->SinceGone += aDelta;
        TESObjectREFR* pGone = TESObjectREFR::GetByHandle(it->Handle);
        if (!pGone)
        {
            it = s_strangerGone.erase(it);
            continue;
        }
        if (!aBusy && it->SincePlaced >= kStrangerDeleteAfter && it->SinceGone >= kStrangerLeaveFade)
        {
            pGone->Delete();
            it = s_strangerGone.erase(it);
            continue;
        }
        ++it;
    }

    s_strangerSweep += aDelta;
    if (s_strangerSweep < kStrangerSweepEvery)
        return;
    const float elapsed = s_strangerSweep;
    s_strangerSweep = 0.f;
    ProcessLists* pLists = ProcessLists::Get();
    if (!pLists)
        return;
    std::vector<std::pair<uint32_t, float>> strays;
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
    {
        const uint32_t handle = pLists->highActorHandleArray[i];
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(handle));
        if (!pActor || !Vats76Service::IsStranger(pActor) || pActor->GetExtension()->IsRemote() || handle == s_visit.Stranger)
            continue;
        if (std::any_of(s_strangerGone.begin(), s_strangerGone.end(), [handle](const StrangerGone& acGone) { return acGone.Handle == handle; }))
            continue;
        float seen = elapsed;
        for (const auto& [strayHandle, strayFor] : s_strangerStrays)
        {
            if (strayHandle == handle)
                seen += strayFor;
        }
        if (!aBusy && seen >= kStrangerDeleteAfter)
        {
            spdlog::info("RD VATS 76': Mysterious Stranger: {:X} isn't anyone's visit (from a save, or a player who left), deleted", pActor->formID);
            pActor->Delete();
            continue;
        }
        strays.emplace_back(handle, seen);
    }
    s_strangerStrays = std::move(strays);
}
} // namespace

// --- The service ---

Vats76Service::Vats76Service(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&Vats76Service::OnUpdate>(this);
    s_pService = this;
    if (!s_pLayer)
    {
        s_pLayer = new LabelsLayer();
        HudMarker::AddLayer(s_pLayer);
    }
    using TDeathEvents = EventDispatcher<TESDeathEvent>*();
    POINTER_FALLOUT4(TDeathEvents, s_deathEvents, kDeathEvents);
    if (auto* pDeathEvents = s_deathEvents.Get()())
        pDeathEvents->RegisterSink(&s_deathSink);
}

Vats76Service::~Vats76Service() noexcept
{
    s_pService = nullptr;
    s_active.store(false);
}

bool Vats76Service::IsActive() noexcept
{
    return s_active.load();
}

bool Vats76Service::TakesVatsButton() noexcept
{
    return s_pService && s_installed.load() && s_pService->m_settings.Enabled;
}

void Vats76Service::OnVatsButton() noexcept
{
    if (!s_pService)
        return;
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        s_pService->Toggle(pPlayer);
}

void Vats76Service::OnAttackStarted() noexcept
{
    s_attackStarted.store(true);
}

bool Vats76Service::SwallowsAttackRelease() noexcept
{
    // the whole burst goes out once started, as in VATS
    return s_active.load() && s_burstRounds.load() > 0;
}

bool Vats76Service::SwallowsAttackStart() noexcept
{
    return s_active.load() && (s_waitForRelease.load() || s_burstRounds.load() > 0);
}

void Vats76Service::AimLaunch(ProjectileLaunchData& aData) noexcept
{
    t_criticalLaunch = false;
    if (!s_active.load() || !aData.pShooter || aData.pShooter != PlayerCharacter::Get())
        return;
    // thrown grenades and placed mines go as thrown
    const uint8_t type = WeaponType(aData.FromWeapon.pWeapon, aData.FromWeapon.pWeaponData);
    if (type == kWeaponTypeGrenade || type == kWeaponTypeMine)
        return;

    std::scoped_lock _(s_shotLock);
    if (!s_shot.Valid || !s_shot.pTarget)
        return;

    // one roll per attack: all of a shotgun's pellets, and a whole burst
    const auto now = std::chrono::steady_clock::now();
    const uint32_t burst = s_burstRounds.load() > 0 || s_burstDone.load() ? s_burstId.load() : 0;
    const bool sameBurst = burst != 0 && burst == s_lastLaunchBurst;
    const bool sameShot = now - s_lastLaunch < kSameShot;
    if (!sameBurst && !sameShot)
    {
        ++s_attack;
        std::uniform_real_distribution<float> roll(0.f, 1.f);
        s_attackRoll = roll(s_random);
        const int32_t chainIndex = NoteAttack(s_shot.Handle, s_shot.Limb);
        if (Cast<Actor>(s_shot.pTarget))
            QueueStrangerRoll(s_shot.Handle);
        WeaponView weapon{(TESForm*)aData.FromWeapon.pWeapon, (void*)aData.FromWeapon.pWeaponData};
        // Gun Fu's top rank makes it critical on its own, and the meter's kept
        const bool gunFu = s_perksOn.load() && GunFuCritical(PlayerCharacter::Get(), weapon, s_shot.pTarget, chainIndex);
        // As the game's VATS does with an attack as it goes: a readied critical strike always hits and is spent; any other
        // attack that will hit gives the meter its charge
        s_attackCritical = gunFu || s_criticalReady.exchange(false);
        if (gunFu)
            spdlog::info("RD VATS 76': Gun Fu makes the attack on target #{} critical", chainIndex + 1);
        else if (s_attackCritical)
            s_strikesOwed.fetch_add(1);
        else if (s_attackRoll < s_shot.Chance)
            s_chargesOwed.fetch_add(1);
    }
    const bool newRound = !sameShot;
    s_lastLaunch = now;
    s_lastLaunchBurst = burst;

    const bool hit = s_attackCritical || s_attackRoll < s_shot.Chance;
    t_criticalLaunch = s_attackCritical;

    const glm::vec3 origin(aData.Origin.x, aData.Origin.y, aData.Origin.z);
    glm::vec3 direction = s_shot.Point - origin;
    if (!hit)
        direction = MissDirection(direction, s_shot.Radius);

    using TAngles = void(const NiPoint3*, float*, float*);
    POINTER_FALLOUT4(TAngles, s_angles, kDirectionToAngles);
    const NiPoint3 line(direction);
    s_angles.Get()(&line, &aData.fZAngle, &aData.fXAngle);

    aData.pHomingTarget = s_shot.pTarget;
    aData.bAlwaysHit = hit;
    aData.bIntentionalMiss = !hit;
    if (hit && s_shot.Limb != 0xFFFFFFFF)
        aData.eTargetLimb = static_cast<int32_t>(s_shot.Limb);

    // the first shots of a session, to read back what it did
    static std::atomic<int> s_logged{0};
    if (newRound && s_logged.fetch_add(1) < 40)
        spdlog::info("RD VATS 76': shot at {:X} limb {} chance {:.0f}% roll {:.0f}% {}{}{}", s_shot.pTarget->formID, s_shot.Limb, s_shot.Chance * 100.f, s_attackRoll * 100.f, hit ? "hit" : "miss", s_attackCritical ? " (critical strike)" : "",
                     burst ? " (burst)" : "");

    // a round of a burst: the last one ends it
    if (newRound && burst != 0)
    {
        if (s_burstRounds.fetch_sub(1) <= 1)
            s_burstDone.store(true);
    }
}

void Vats76Service::OnMainLoop() noexcept
{
    if (const uint32_t target = s_lungeWanted.exchange(0))
        TryLunge(target);
}

void Vats76Service::OnLaunched(uint32_t aProjectileHandle) noexcept
{
    if (!std::exchange(t_criticalLaunch, false) || !aProjectileHandle)
        return;
    // the game's own critical flag on the projectile: its hits are critical, as a VATS critical strike's are
    if (TESObjectREFR* pProjectile = TESObjectREFR::GetByHandle(aProjectileHandle))
        *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(pProjectile) + kProjectileFlags) |= kProjectileCritical;
}

bool Vats76Service::TakeJump(bool aDown, bool aJustPressed) noexcept
{
    // a press taken is swallowed until it's let go (the jump control gets it every frame it's down)
    if (s_jumpTaken)
    {
        if (!aDown)
            s_jumpTaken = false;
        return true;
    }
    if (!aJustPressed || !s_pService || !s_active.load())
        return false;
    // the jump key is the critical key when none is set, or when the one set is the same key
    const int key = s_pService->m_criticalKey;
    if (key != 0 && !((GetAsyncKeyState(key) & 0x8000) != 0 && ModifiersDown(s_pService->m_criticalModifiers)))
        return false;
    // nothing to ready or cancel: it's a jump
    if (!s_criticalReady.load() && !CriticalFull(PlayerCharacter::Get()))
        return false;
    s_pService->ToggleCritical();
    s_pService->m_criticalKeyDown = true;
    s_jumpTaken = true;
    return true;
}

void Vats76Service::UpdateSettings(float aDelta) noexcept
{
    m_settingsTimer -= aDelta;
    if (m_settingsTimer > 0.f)
        return;
    m_settingsTimer = kSettingsInterval;

    const auto path = (std::filesystem::current_path() / L"Data" / kSettingsFile).wstring();
    const auto readFloat = [&path](const wchar_t* apKey, float aDefault)
    {
        wchar_t text[32]{};
        GetPrivateProfileStringW(kSettingsSection, apKey, L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
        wchar_t* pEnd = nullptr;
        const float value = std::wcstof(text, &pEnd);
        return pEnd != text && std::isfinite(value) ? value : aDefault;
    };
    const auto readBool = [&readFloat](const wchar_t* apKey, bool aDefault) { return readFloat(apKey, aDefault ? 1.f : 0.f) != 0.f; };

    m_settings.Enabled = readBool(L"bEnabled", true);
    m_settings.MaxHitChance = std::clamp(readFloat(L"fMaxHitChance", 95.f) / 100.f, 0.f, 1.f);
    m_settings.Bursts = readBool(L"bBursts", true);
    m_settings.MeleeJump = readBool(L"bMeleeJump", true);
    s_meleeJumpOn.store(m_settings.MeleeJump);
    m_settings.Perks = readBool(L"bPerks", true);
    s_perksOn.store(m_settings.Perks);
    m_settings.Sounds = readBool(L"bSounds", true);
    m_settings.ExcludeCompanions = readBool(L"bExcludeCompanions", true);
    m_settings.ExcludeDogmeat = readBool(L"bExcludeDogmeat", true);
    m_settings.ExcludeSettlers = readBool(L"bExcludeSettlers", true);
    m_settings.TargetExplosives = readBool(L"bTargetExplosives", true);
    m_settings.EndDelay = std::max(readFloat(L"fEndDelay", 1.5f), 0.f);
    m_settings.ShowAllParts = readBool(L"bShowAllParts", true);
    m_settings.HighlightPart = readBool(L"bHighlightPart", true);
    m_settings.CriticalReadyMessage = readBool(L"bCriticalReadyMessage", true);
    m_settings.Stranger = readBool(L"bMysteriousStranger", true);
    ReadCriticalKey();
}

void Vats76Service::ReadCriticalKey() noexcept
{
    // MCM writes the file as its menu closes: read again when it's changed
    const auto path = std::filesystem::current_path() / L"Data" / kKeybindsFile;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        m_criticalKey = 0;
        m_criticalModifiers = 0;
        return;
    }
    if (time == m_keybindsTime)
        return;
    m_keybindsTime = time;

    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    int key = 0;
    uint32_t modifiers = 0;
    if (!FindKeybind(text.str(), key, modifiers))
    {
        key = 0;
        modifiers = 0;
    }
    if (key != m_criticalKey || modifiers != m_criticalModifiers)
        spdlog::info("RD VATS 76': critical key {} (modifiers {})", key ? fmt::format("{:#x}", key) : std::string("jump"), modifiers);
    m_criticalKey = key;
    m_criticalModifiers = modifiers;
}

void Vats76Service::ToggleCritical() noexcept
{
    // pressed again before the attack: put back, the meter stays full
    if (s_criticalReady.exchange(false))
    {
        ShowMessage("Critical strike cancelled");
        spdlog::info("RD VATS 76': critical strike cancelled");
        return;
    }
    if (!CriticalFull(PlayerCharacter::Get()))
    {
        ShowMessage("Critical meter not full");
        PlaySound(m_settings.Sounds, "UIVATSInsufficientAP");
        return;
    }
    s_criticalReady.store(true);
    ShowMessage("Next attack is a critical strike");
    PlaySound(m_settings.Sounds, "UIVATSSelectPart");
    spdlog::info("RD VATS 76': critical strike readied");
}

void Vats76Service::UpdateCritical(PlayerCharacter* apPlayer) noexcept
{
    // the kill award's full meter
    if (s_fillCriticalOwed.exchange(false))
    {
        POINTER_FALLOUT4(float, s_limit, kCriticalChargeLimit);
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(apPlayer) + kCriticalCharge) = *s_limit.Get();
    }
    // the game's entries inside these ask whether this is VATS
    const AttackScope meterScope;
    const ScopeGuard meterGuard(&meterScope);
    // what the attacks owe the meter, on this thread as the game's VATS does it: a strike spent, then the charge of each hit
    for (int32_t strikes = s_strikesOwed.exchange(0); strikes > 0; --strikes)
    {
        using TSpend = void();
        POINTER_FALLOUT4(TSpend, s_spend, kSpendCritical);
        s_spend.Get()();
        spdlog::info("RD VATS 76': critical strike spent");
    }
    for (int32_t charges = s_chargesOwed.exchange(0); charges > 0; --charges)
    {
        using TCharge = void();
        POINTER_FALLOUT4(TCharge, s_charge, kAddCriticalCharge);
        s_charge.Get()();
    }

    const bool full = CriticalFull(apPlayer);
    if (s_active.load())
    {
        // the moment it fills: the game's VATS note and sound
        if (full && !m_criticalWasFull && m_settings.CriticalReadyMessage)
        {
            ShowMessage("Critical strike ready");
            PlaySound(m_settings.Sounds, "UIVATSCriticalAvailable");
        }
        // a set key other than jump (jump's own press comes through TakeJump)
        if (m_criticalKey != 0)
        {
            const bool down = GameHasKeyboard() && (GetAsyncKeyState(m_criticalKey) & 0x8000) != 0 && ModifiersDown(m_criticalModifiers);
            if (down && !m_criticalKeyDown)
                ToggleCritical();
            m_criticalKeyDown = down;
        }
    }
    // spent by other means (the VATS menu, a perk): nothing left to strike with
    if (!full && s_criticalReady.exchange(false))
        spdlog::info("RD VATS 76': readied critical strike gone, the meter isn't full");
    m_criticalWasFull = full;
}

void Vats76Service::UpdateHighlight(TESObjectREFR* apTarget) noexcept
{
    if (!apTarget || !m_settings.HighlightPart || m_part < 0 || m_part >= static_cast<int>(m_parts.size()))
    {
        ClearGlow();
        return;
    }
    SetGlow(Get3D(apTarget), m_parts[m_part].Types);
}

bool Vats76Service::PartBox(const Part& acPart, float& aX, float& aY, float& aWidth, float& aHeight) const noexcept
{
    float clip[4][4]{};
    if (!HudMarker::GetWorldToClip(clip))
        return false;
    // the camera's right and up in the world (the first two rows' directions)
    const auto axis = [&clip](int aRow) { return glm::normalize(glm::vec3(clip[aRow][0], clip[aRow][1], clip[aRow][2])); };
    const glm::vec3 right = axis(0);
    const glm::vec3 up = axis(1);

    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float maxX = -minX;
    float maxY = -minX;
    bool any = false;
    const auto add = [&](const glm::vec3& acPoint, float aThickness)
    {
        for (const glm::vec3& offset : {right * aThickness, -right * aThickness, up * aThickness, -up * aThickness})
        {
            float x = 0.f;
            float y = 0.f;
            if (!HudMarker::WorldToScreen(NiPoint3(acPoint + offset), x, y))
                continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            any = true;
        }
    };
    // each of its bones, from its node to where its child bones start, as thick as that kind of part
    for (size_t i = 0; i < acPart.Nodes.size(); ++i)
    {
        uint8_t* pNode = acPart.Nodes[i];
        const float thickness = PartThickness(i < acPart.Types.size() ? acPart.Types[i] : 0xFF);
        add(NodeTools::GetWorldPosition(pNode), thickness);
        NodeTools::ForEachChild(pNode, [&](uint8_t* apChild) { add(NodeTools::GetWorldPosition(apChild), thickness); });
    }
    if (!any)
        return false;
    aX = (minX + maxX) * 0.5f;
    aY = (minY + maxY) * 0.5f;
    aWidth = maxX - minX;
    aHeight = maxY - minY;
    return true;
}

void Vats76Service::Toggle(PlayerCharacter* apPlayer) noexcept
{
    if (s_active.load())
    {
        TurnOff("the button", true);
        return;
    }
    TurnOn(apPlayer);
}

void Vats76Service::TurnOn(PlayerCharacter* apPlayer) noexcept
{
    if (apPlayer->IsDead())
        return;

    // it won't start without the Action Points for an attack
    WeaponInstance weapon;
    GetPlayerWeapon(apPlayer, weapon);
    const float cost = AttackCost(apPlayer, weapon, nullptr);
    if (cost > 0.f && apPlayer->GetActorValue(ActorValueInfo::kActionPoints) < cost)
    {
        ShowMessage("Out of Action Points");
        PlaySound(m_settings.Sounds, "UIVATSInsufficientAP");
        return;
    }

    m_target = 0;
    m_partsTarget = 0;
    m_parts.clear();
    m_part = -1;
    m_noTargetTime = 0.f;
    m_refreshTimer = 0.f;
    m_waitingForFirst = true;
    s_burstRounds.store(0);
    s_burstDone.store(false);
    s_waitForRelease.store(false);
    s_attackStarted.store(false);
    s_criticalReady.store(false);
    m_criticalWasFull = CriticalFull(apPlayer);
    m_criticalKeyDown = true;
    s_strangerNewSession.store(true);
    s_active.store(true);
    ForceShowMeters(true);
    PlaySound(m_settings.Sounds, "UIVATSEnter");
    spdlog::info("RD VATS 76': on, cost of an attack {:.1f} AP", cost);
}

void Vats76Service::TurnOff(const char* apWhy, bool aSound, bool aStrangerStays) noexcept
{
    if (!s_active.exchange(false))
        return;
    // the player turned it off: a Mysterious Stranger on his way, or here, goes too
    if (!aStrangerStays)
        s_strangerCut.store(true);
    {
        std::scoped_lock _(s_shotLock);
        s_shot = Shot{};
    }
    // a burst under way ends with the rest of its Action Points back
    if (const int32_t left = s_burstRounds.exchange(0); left > 0)
        AddApOwed(-static_cast<float>(left) * s_burstRoundCost.load());
    s_burstDone.store(false);
    s_waitForRelease.store(false);
    // a readied strike not used: put back, the meter stays full
    if (s_criticalReady.exchange(false))
        spdlog::info("RD VATS 76': readied critical strike put back");
    ForceShowMeters(false);
    ClearGlow();
    m_target = 0;
    m_partsTarget = 0;
    m_parts.clear();
    m_part = -1;
    if (s_pLayer)
        s_pLayer->SetWanted({});
    if (aSound)
        PlaySound(m_settings.Sounds, "UIVATSExitSD");
    spdlog::info("RD VATS 76': off ({})", apWhy);
}

void Vats76Service::RefreshCandidates(PlayerCharacter* apPlayer) noexcept
{
    m_candidates.clear();

    using TCheck = bool(Actor*);
    using TTargetable = bool(const uint32_t*, bool);
    using TCanSee = bool(Actor*, TESObjectREFR*, uint8_t*);
    using THostile = bool(Actor*, Actor*);
    using TLinkedRef = TESObjectREFR*(Actor*, TESForm*);
    POINTER_FALLOUT4(TTargetable, s_targetable, kVatsTargetable);
    POINTER_FALLOUT4(TCanSee, s_canSee, kCanSee);
    POINTER_FALLOUT4(THostile, s_hostile, kIsHostileToActor);
    POINTER_FALLOUT4(TCheck, s_isDogmeat, kIsDogmeat);
    POINTER_FALLOUT4(TCheck, s_isTeammate, kIsPlayerTeammate);
    POINTER_FALLOUT4(TLinkedRef, s_linkedRef, kGetLinkedRef);
    TESForm* pWorkshopLink = TESForm::GetById(kWorkshopItemKeyword);

    const glm::vec3 playerPosition(apPlayer->position.x, apPlayer->position.y, apPlayer->position.z);
    ProcessLists* pLists = ProcessLists::Get();
    if (!pLists)
        return;
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
    {
        const uint32_t handle = pLists->highActorHandleArray[i];
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(handle));
        if (!pActor || pActor == apPlayer || pActor->IsDead() || !pActor->GetNiNode())
            continue;
        if (!s_targetable.Get()(&handle, true))
            continue;
        const bool hostile = s_hostile.Get()(pActor, apPlayer);
        if (!hostile)
        {
            // the other players' characters are nobody's targets unless they're fighting
            if (pActor->GetExtension()->IsRemotePlayer())
                continue;
            if (m_settings.ExcludeDogmeat && s_isDogmeat.Get()(pActor))
                continue;
            if (m_settings.ExcludeCompanions && s_isTeammate.Get()(pActor))
                continue;
            if (m_settings.ExcludeSettlers && pWorkshopLink && s_linkedRef.Get()(pActor, pWorkshopLink))
                continue;
        }
        const glm::vec3 position(pActor->position.x, pActor->position.y, pActor->position.z);
        if (glm::distance(playerPosition, position) > kDefaultEngageDistance)
            continue;

        NiPoint3 centre(glm::vec3(position.x, position.y, position.z + 64.f));
        float x = 0.f;
        float y = 0.f;
        if (!HudMarker::WorldToScreen(centre, x, y))
            continue;
        const float screen = std::hypot(x - 0.5f, y - 0.5f);
        if (screen > kMaxScreenDistance)
            continue;
        uint8_t out = 0;
        if (!s_canSee.Get()(apPlayer, pActor, &out))
            continue;
        m_candidates.push_back({handle, screen, hostile});
    }

    // Mines and thrown grenades, as the game's VATS can take them
    if (!m_settings.TargetExplosives)
        return;
    using TExplosiveTargetable = bool(const uint32_t*, bool);
    POINTER_FALLOUT4(TExplosiveTargetable, s_explosiveTargetable, kExplosiveTargetable);
    static std::vector<uint32_t> s_explosives;
    ListExplosives(s_explosives);
    for (const uint32_t handle : s_explosives)
    {
        TESObjectREFR* pExplosive = TESObjectREFR::GetByHandle(handle);
        if (!pExplosive || !Get3D(pExplosive) || !s_explosiveTargetable.Get()(&handle, false))
            continue;
        const glm::vec3 centre = BoundCentre(pExplosive);
        if (glm::distance(playerPosition, centre) > kDefaultEngageDistance)
            continue;
        float x = 0.f;
        float y = 0.f;
        if (!HudMarker::WorldToScreen(NiPoint3(centre), x, y))
            continue;
        const float screen = std::hypot(x - 0.5f, y - 0.5f);
        if (screen > kMaxScreenDistance)
            continue;
        uint8_t out = 0;
        if (!s_canSee.Get()(apPlayer, pExplosive, &out))
            continue;
        m_candidates.push_back({handle, screen, false});
    }
}

void Vats76Service::PickTarget(PlayerCharacter* apPlayer) noexcept
{
    const auto score = [](const Candidate& acCandidate) { return acCandidate.ScreenDistance - (acCandidate.Hostile ? kHostileBonus : 0.f); };
    const Candidate* pBest = nullptr;
    const Candidate* pCurrent = nullptr;
    for (const Candidate& candidate : m_candidates)
    {
        if (candidate.Handle == m_target)
            pCurrent = &candidate;
        if (!pBest || score(candidate) < score(*pBest))
            pBest = &candidate;
    }
    if (pCurrent && pBest && score(*pBest) > score(*pCurrent) - kKeepTargetMargin)
        pBest = pCurrent;

    const uint32_t wanted = pBest ? pBest->Handle : 0;
    if (wanted != m_target)
    {
        m_target = wanted;
        if (wanted != 0)
        {
            m_waitingForFirst = false;
            PlaySound(m_settings.Sounds, "UIVATSSelectTarget");
        }
    }
}

void Vats76Service::BuildParts(Actor* apTarget) noexcept
{
    // the part picked keeps its pick when it's still there
    const std::string picked = m_part >= 0 && m_part < static_cast<int>(m_parts.size()) ? m_parts[m_part].Name : std::string{};
    m_parts.clear();
    m_part = -1;
    using TGetData = const uint8_t*(Actor*);
    using TDismembered = bool(Actor*, uint32_t);
    POINTER_FALLOUT4(TGetData, s_getData, kGetBodyPartData);
    POINTER_FALLOUT4(TDismembered, s_dismembered, kIsDismembered);
    const uint8_t* pData = s_getData.Get()(apTarget);
    auto* pRoot = reinterpret_cast<uint8_t*>(apTarget->GetNiNode());
    m_partsRoot = pRoot;
    if (!pData || !pRoot)
        return;

    for (uintptr_t at = kPartsBegin; at < kPartsEnd; at += sizeof(void*))
    {
        const auto* pPart = *reinterpret_cast<const uint8_t* const*>(pData + at);
        if (!pPart)
            continue;
        const uint8_t type = *(pPart + kPartType);
        if (type == kPartTypeWhole)
            continue;
        const auto* pValue = *reinterpret_cast<const uint8_t* const*>(pPart + kPartLimbValue);
        if (!pValue || *reinterpret_cast<const int32_t*>(pValue + kValueType) != kValueTypeCondition)
            continue;
        if (s_dismembered.Get()(apTarget, type))
            continue;
        const auto& nodeName = *reinterpret_cast<const BSFixedString*>(pPart + kPartNodeName);
        uint8_t* pNode = NodeTools::GetObjectByName(pRoot, nodeName);
        if (!pNode)
            continue;
        const auto& displayName = *reinterpret_cast<const BSFixedString*>(pPart + kPartDisplayName);
        const char* pName = displayName.AsAscii();
        std::string name = pName && *pName ? pName : "";
        // one entry per name: a part in several segments has them all (for its box and its glow)
        const auto same = std::find_if(m_parts.begin(), m_parts.end(), [&name](const Part& acOther) { return !name.empty() && acOther.Name == name; });
        if (same != m_parts.end())
        {
            same->Nodes.push_back(pNode);
            same->Types.push_back(type);
            continue;
        }
        Part part{pPart, std::move(name), pNode, type, 0.f};
        part.Nodes.push_back(pNode);
        part.Types.push_back(type);
        m_parts.push_back(std::move(part));
    }

    if (!picked.empty())
        for (int i = 0; i < static_cast<int>(m_parts.size()); ++i)
            if (m_parts[i].Name == picked)
                m_part = i;
}

void Vats76Service::BuildExplosivePart(TESObjectREFR* apTarget) noexcept
{
    m_parts.clear();
    m_part = -1;
    auto* pRoot = const_cast<uint8_t*>(static_cast<const uint8_t*>(Get3D(apTarget)));
    m_partsRoot = pRoot;
    if (!pRoot)
        return;
    // no body parts: one spot, the whole thing (no part type for the glow to go on)
    Part part{nullptr, "Explosive", pRoot, 0xFFFFFFFF, 0.f};
    part.Nodes.push_back(pRoot);
    m_parts.push_back(std::move(part));
}

void Vats76Service::RefreshChances(PlayerCharacter* apPlayer, TESObjectREFR* apTarget) noexcept
{
    WeaponInstance weapon;
    GetPlayerWeapon(apPlayer, weapon);
    WeaponView view{weapon.pForm, weapon.pData};
    AttackScope scope;
    scope.pWeapon = weapon.pForm;
    scope.pTarget = apTarget;
    scope.WeaponType = WeaponType(weapon.pForm, weapon.pData);
    const bool perks = s_perksOn.load();

    // Concentrated Fire: each attack in a row on a part adds the perk's points to that part's chance
    float streakBonus = 0.f;
    uint32_t streakLimb = 0xFFFFFFFF;
    if (perks)
    {
        int32_t streak = 0;
        {
            std::scoped_lock _(s_perkLock);
            if (s_streakTarget == m_target)
            {
                streak = s_streak;
                streakLimb = s_streakLimb;
            }
        }
        if (streak > 0)
        {
            const ScopeGuard guard(&scope);
            Actor* pOwner = apPlayer;
            float perAttack = 0.f;
            PerkEntry(kEntryConcentratedFire, pOwner, &view, &pOwner, &perAttack);
            if (perAttack > 0.f)
                streakBonus = perAttack * 0.01f * static_cast<float>(streak);
        }
    }

    using TChance = float(TESObjectREFR*, WeaponArg*, const void*, float);
    POINTER_FALLOUT4(TChance, s_chance, kHitChance);
    for (Part& part : m_parts)
    {
        scope.Limb = part.Limb == 0xFFFFFFFF ? -1 : static_cast<int32_t>(part.Limb);
        const ScopeGuard guard(perks ? &scope : t_scope);
        WeaponArg arg(weapon);
        const float chance = s_chance.Get()(apTarget, &arg, part.pPart, 1.f) + (part.Limb == streakLimb ? streakBonus : 0.f);
        part.Chance = std::clamp(chance, 0.f, m_settings.MaxHitChance);
    }
}

void Vats76Service::PickPart() noexcept
{
    float best = std::numeric_limits<float>::max();
    int picked = -1;
    for (int i = 0; i < static_cast<int>(m_parts.size()); ++i)
    {
        const glm::vec3 at = NodeTools::GetWorldPosition(m_parts[i].pNode);
        float x = 0.f;
        float y = 0.f;
        if (!HudMarker::WorldToScreen(NiPoint3(at), x, y))
            continue;
        const float distance = std::hypot(x - 0.5f, y - 0.5f);
        if (distance < best)
        {
            best = distance;
            picked = i;
        }
    }
    if (picked != m_part)
    {
        if (m_part >= 0 && picked >= 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastPartSound >= kPartSoundGap)
            {
                m_lastPartSound = now;
                PlaySound(m_settings.Sounds, "UIVATSSelectPart");
            }
        }
        m_part = picked;
    }
}

void Vats76Service::ShowOnHud(TESObjectREFR* apTarget) noexcept
{
    std::vector<Label> labels;
    if (apTarget)
    {
        for (int i = 0; i < static_cast<int>(m_parts.size()) && labels.size() < kMaxLabels; ++i)
        {
            const bool selected = i == m_part;
            if (!selected && !m_settings.ShowAllParts)
                continue;
            const Part& part = m_parts[i];
            const glm::vec3 at = NodeTools::GetWorldPosition(part.pNode);
            float x = 0.f;
            float y = 0.f;
            if (!HudMarker::WorldToScreen(NiPoint3(at), x, y))
                continue;
            // the picked part's bracket round the part as it is on the screen
            float width = 0.f;
            float height = 0.f;
            if (selected)
            {
                float boxX = 0.f;
                float boxY = 0.f;
                if (PartBox(part, boxX, boxY, width, height))
                {
                    x = boxX;
                    y = boxY;
                }
            }
            const int percent = static_cast<int>(std::round(part.Chance * 100.f));
            labels.push_back({selected && !part.Name.empty() ? fmt::format("{} {}%", part.Name, percent) : fmt::format("{}%", percent), x, y, width, height, selected});
        }
    }
    if (s_pLayer)
        s_pLayer->SetWanted(std::move(labels));
}

void Vats76Service::PublishShot(TESObjectREFR* apTarget) noexcept
{
    Shot shot;
    if (apTarget && m_part >= 0 && m_part < static_cast<int>(m_parts.size()))
    {
        const Part& part = m_parts[m_part];
        shot.Valid = true;
        shot.pTarget = apTarget;
        shot.Handle = m_target;
        shot.Point = NodeTools::GetWorldPosition(part.pNode);
        shot.Limb = part.Limb;
        shot.Chance = part.Chance;
    }
    std::scoped_lock _(s_shotLock);
    s_shot = shot;
}

void Vats76Service::UpdateBurst(PlayerCharacter* apPlayer) noexcept
{
    // the last round's out: the weapon stops, and the button has to be let go before the next attack
    if (s_burstDone.exchange(false))
    {
        s_burstRounds.store(0);
        using TStop = void(PlayerCharacter*);
        POINTER_FALLOUT4(TStop, s_stop, kStopFiring);
        s_stop.Get()(apPlayer);
        s_waitForRelease.store(true);
        spdlog::debug("RD VATS 76': burst over");
    }

    if (!s_attackStarted.exchange(false))
        return;

    // A new attack: its Action Points are taken now, a whole burst's at once
    WeaponInstance weapon;
    GetPlayerWeapon(apPlayer, weapon);
    TESObjectREFR* pTarget = m_target ? TESObjectREFR::GetByHandle(m_target) : nullptr;
    const float cost = AttackCost(apPlayer, weapon, pTarget);
    if (cost > 0.f && apPlayer->GetActorValue(ActorValueInfo::kActionPoints) < cost)
    {
        ShowMessage("Out of Action Points");
        PlaySound(m_settings.Sounds, "UIVATSInsufficientAP");
        TurnOff("out of Action Points", true, true);
        return;
    }
    AddApOwed(cost);

    // a melee attack jumps to the target when it's out of reach
    if (m_settings.MeleeJump && pTarget && IsMeleeWeapon(weapon.pForm, weapon.pData))
        s_lungeWanted.store(m_target);

    const int32_t rounds = m_settings.Bursts && pTarget ? BurstRounds(weapon) : 0;
    if (rounds >= 2)
    {
        s_burstRoundCost.store(cost / static_cast<float>(rounds));
        s_burstId.fetch_add(1);
        s_burstRounds.store(rounds);
        s_burstDone.store(false);
        spdlog::debug("RD VATS 76': burst of {}, {:.1f} AP", rounds, cost);
    }
}

void Vats76Service::PayActionPoints(PlayerCharacter* apPlayer) noexcept
{
    const float owed = s_apOwed.exchange(0.f);
    if (owed == 0.f)
        return;
    // (the value it's to have; what's owed is negative when a burst gives some back)
    const float maximum = apPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints);
    const float value = std::clamp(apPlayer->GetActorValue(ActorValueInfo::kActionPoints) - owed, 0.f, maximum);
    apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kActionPoints, value);
}

bool Vats76Service::IsStranger(const Actor* apActor) noexcept
{
    return apActor && apActor->IsTemporary() && apActor->baseForm && apActor->baseForm->formID == kStrangerNpc;
}

void Vats76Service::RollStranger(PlayerCharacter* apPlayer, uint32_t aTarget) noexcept
{
    Actor* pTarget = Cast<Actor>(TESObjectREFR::GetByHandle(aTarget));
    if (!pTarget || pTarget->IsDead() || !pTarget->GetNiNode() || pTarget->GetExtension()->IsRemotePlayer())
        return;
    if (std::find(s_strangerRolled.begin(), s_strangerRolled.end(), aTarget) != s_strangerRolled.end())
        return;
    // the game's: the perk, at any rank (its global has the odds), and a target low on health
    TESForm* pPerk = DefaultObjectForm(kStrangerPerkObject, kFormTypePerk);
    auto* pOdds = static_cast<TESGlobal*>(DefaultObjectForm(kStrangerOddsObject, kFormTypeGlobal));
    if (!pPerk || !pOdds || apPlayer->GetPerkRank(pPerk->formID) == 0)
        return;
    POINTER_FALLOUT4(int32_t, s_maxHealth, kStrangerMaxHealth);
    POINTER_FALLOUT4(float, s_maxShare, kStrangerMaxHealthShare);
    const float health = pTarget->GetActorValue(ActorValueInfo::kHealth);
    const float full = pTarget->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float share = full > 0.f ? health / full : 1.f;
    if (health > static_cast<float>(*s_maxHealth.Get()) && share > *s_maxShare.Get())
        return;

    // one roll a target, as the plugin has it
    s_strangerRolled.push_back(aTarget);
    float roll = 1.f;
    {
        std::scoped_lock _(s_shotLock);
        roll = std::uniform_real_distribution<float>(0.f, 1.f)(s_random);
    }
    const float odds = pOdds->f;
    const bool appears = roll <= odds;
    spdlog::info("RD VATS 76': Mysterious Stranger: roll for {:X} at {:.0f} health ({:.0f}%), chance {:.2f} -> {}", pTarget->formID, health, share * 100.f, odds, appears ? "appears" : "no");
    if (!appears)
        return;

    s_visit = StrangerVisit{};
    s_visit.Step = StrangerStep::kFindingSpot;
    s_visit.Target = aTarget;
    // the first look for a spot is now
    s_visit.SinceSpotTry = kStrangerSpotEvery;
    s_strangerTargetDied.store(false);
    s_strangerKilled.store(false);
    s_strangerHandle.store(0);
    s_strangerTarget.store(aTarget);
}

void Vats76Service::UpdateStranger(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const bool busy = NemesisGame::IsBusy();
    SweepStrangers(aDelta, busy);
    if (s_strangerNewSession.exchange(false))
        s_strangerRolled.clear();

    StrangerVisit& visit = s_visit;
    const bool on = m_settings.Enabled && m_settings.Stranger;
    const bool cut = s_strangerCut.exchange(false);
    if (cut || !on || apPlayer->IsDead())
    {
        {
            std::scoped_lock _(s_strangerLock);
            s_strangerRolls.clear();
        }
        if (visit.Step != StrangerStep::kNone)
            EndStrangerVisit(visit, apPlayer->IsDead() ? "the player died" : on ? "VATS 76' was turned off" : "switched off");
        return;
    }

    if (visit.Step != StrangerStep::kNone)
    {
        visit.Time += aDelta;
        if (visit.Stranger)
            visit.SincePlaced += aDelta;
    }
    Actor* pTarget = visit.Target ? Cast<Actor>(TESObjectREFR::GetByHandle(visit.Target)) : nullptr;
    Actor* pStranger = visit.Stranger ? Cast<Actor>(TESObjectREFR::GetByHandle(visit.Stranger)) : nullptr;
    const bool targetGone = !pTarget || pTarget->IsDead() || !pTarget->GetNiNode();

    switch (visit.Step)
    {
    case StrangerStep::kNone: break;
    case StrangerStep::kFindingSpot:
    {
        if (targetGone)
        {
            EndStrangerVisit(visit, "the target's gone");
            break;
        }
        visit.SinceSpotTry += aDelta;
        if (!busy && visit.SinceSpotTry >= kStrangerSpotEvery)
        {
            visit.SinceSpotTry = 0.f;
            NiPoint3 spot{};
            if (FindStrangerSpot(apPlayer, pTarget, spot))
            {
                Actor* pNew = PutStrangerAt(spot);
                if (!pNew)
                {
                    EndStrangerVisit(visit, "he couldn't be put in");
                    break;
                }
                visit.Stranger = pNew->GetHandle().handle.iBits;
                s_strangerHandle.store(visit.Stranger);
                visit.Step = StrangerStep::kArriving;
                visit.Time = 0.f;
                visit.SincePlaced = 0.f;
                spdlog::info("RD VATS 76': Mysterious Stranger: warped in next to the player ({:X})", pNew->formID);
                break;
            }
        }
        if (visit.Time >= kStrangerSpotFor)
            EndStrangerVisit(visit, "no free spot near the player with a clear shot");
        break;
    }
    case StrangerStep::kArriving:
    {
        if (!pStranger)
        {
            EndStrangerVisit(visit, "his character's gone from the world");
            break;
        }
        if (targetGone || s_strangerTargetDied.load())
        {
            EndStrangerVisit(visit, "the target's gone");
            break;
        }
        if (visit.Time >= kStrangerArriveAfter && Get3D(pStranger))
        {
            EngageStranger(apPlayer, pStranger, pTarget, visit);
            visit.Step = StrangerStep::kVisiting;
            visit.Time = 0.f;
            break;
        }
        if (visit.Time >= kStrangerArriveBy)
            EndStrangerVisit(visit, "no 3D in time, leaving");
        break;
    }
    case StrangerStep::kVisiting:
    {
        if (!pStranger)
        {
            EndStrangerVisit(visit, "his character's gone from the world");
            break;
        }
        if (s_strangerTargetDied.load())
        {
            const bool his = s_strangerKilled.load();
            spdlog::info("RD VATS 76': Mysterious Stranger: target died ({})", his ? "his kill" : "not his kill");
            if (his)
                PlaySound(true, kStrangerMusicKill);
            visit.LeaveAfter = his ? kStrangerLeavesAfterKill : 0.f;
            visit.Step = StrangerStep::kLeaving;
            visit.Time = 0.f;
            break;
        }
        if (targetGone)
            EndStrangerVisit(visit, "the target's gone");
        else if (visit.Time >= kStrangerStaysFor)
            EndStrangerVisit(visit, "his time's up");
        break;
    }
    case StrangerStep::kLeaving:
    {
        if (!pStranger || visit.Time >= visit.LeaveAfter)
            EndStrangerVisit(visit, s_strangerKilled.load() ? "target killed" : "no kill");
        break;
    }
    }

    // the rolls whose attacks have landed, while nobody's visiting (one visit at a time: the rest are dropped)
    std::vector<uint32_t> due;
    {
        std::scoped_lock _(s_strangerLock);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = s_strangerRolls.begin(); it != s_strangerRolls.end();)
        {
            if (it->Due > now)
            {
                ++it;
                continue;
            }
            due.push_back(it->Target);
            it = s_strangerRolls.erase(it);
        }
    }
    for (const uint32_t target : due)
    {
        if (visit.Step != StrangerStep::kNone || busy)
            break;
        RollStranger(apPlayer, target);
    }
}

void Vats76Service::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    const float delta = static_cast<float>(acEvent.Delta);
    UpdateSettings(delta);

    if (m_standingDown < 0)
        m_standingDown = s_installed.load() ? 0 : 1;
    if (m_standingDown == 1)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    // the attacks' meter charges and strikes come in after it's off too (the last round of a burst)
    UpdateCritical(pPlayer);
    // a Mysterious Stranger finishes his visit after it's off too (out of Action Points, his kill ended it)
    UpdateStranger(pPlayer, delta);
    if (!s_active.load())
        return;
    if (!m_settings.Enabled)
    {
        TurnOff("switched off", false);
        return;
    }
    if (pPlayer->IsDead())
    {
        TurnOff("the player died", false);
        return;
    }
    // the VATS menu, opened by other means: it has the player's aim
    if (UI* pUi = UI::Get(); pUi && pUi->GetMenuOpen(BSFixedString("VATSMenu")))
    {
        TurnOff("the VATS menu opened", false);
        return;
    }

    m_refreshTimer -= delta;
    const bool refresh = m_refreshTimer <= 0.f;
    if (refresh)
    {
        m_refreshTimer = kRefreshInterval;
        RefreshCandidates(pPlayer);
        PickTarget(pPlayer);
    }

    // a person, or a mine or grenade (no actor, one spot)
    TESObjectREFR* pTarget = m_target ? TESObjectREFR::GetByHandle(m_target) : nullptr;
    Actor* pActor = Cast<Actor>(pTarget);
    if (pTarget && (pActor ? pActor->IsDead() || !pActor->GetNiNode() : !Get3D(pTarget)))
    {
        pTarget = nullptr;
        pActor = nullptr;
        m_target = 0;
        m_refreshTimer = 0.f;
    }

    if (!pTarget)
    {
        m_partsTarget = 0;
        m_parts.clear();
        m_part = -1;
        m_noTargetTime += delta;
        // it waits for the first target, then the last one gets a moment to be replaced
        if (!m_waitingForFirst && m_noTargetTime >= m_settings.EndDelay)
        {
            TurnOff("no target left", true, true);
            return;
        }
    }
    else
    {
        m_noTargetTime = 0.f;
        const void* pRoot = pActor ? static_cast<const void*>(pActor->GetNiNode()) : Get3D(pTarget);
        if (m_partsTarget != m_target || m_partsRoot != pRoot)
        {
            m_partsTarget = m_target;
            m_part = -1;
            if (pActor)
                BuildParts(pActor);
            else
                BuildExplosivePart(pTarget);
            RefreshChances(pPlayer, pTarget);
            std::string names;
            for (const Part& part : m_parts)
                names += fmt::format("{}'{}' limb {} {:.0f}%", names.empty() ? "" : ", ", part.Name, part.Limb, part.Chance * 100.f);
            spdlog::info("RD VATS 76': target {:X}: {}", pTarget->formID, names);
        }
        else if (refresh)
        {
            // limbs shot off since drop out, and the nodes are looked up again
            if (pActor)
                BuildParts(pActor);
            RefreshChances(pPlayer, pTarget);
        }
        PickPart();
    }

    PublishShot(pTarget);
    ShowOnHud(pTarget);
    UpdateHighlight(pTarget);
    UpdateBurst(pPlayer);
    PayActionPoints(pPlayer);
}

#endif
