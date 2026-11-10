#pragma once

#if TP_FALLOUT4

struct Actor;
struct TESNPC;
struct TESForm;
struct TESObjectREFR;

// Nemeses (see docs/NEMESIS.md): what NemesisService needs from the game. Every Papyrus native is called the way the
// game binds it (checked in the 1.10.163 exe, NEMESIS.md "Verified internals"), looked up by name, and skipped with a
// log line when the game didn't register it.
namespace NemesisGame
{
// The gang of a character by its factions, under the server's keys (Nemesis::Gangs), "" for none. Generic raiders
// are "raiders" (the server gives them the crew holding the ground). Children of Atom are the Atomites in Winter of Atom.
std::string GangOf(const Actor* apActor, bool aWinterOfAtom) noexcept;
// A person who can become a nemesis: a grown human, ghoul or super mutant, not a player, not essential or unique (story
// characters, companions and named settlers are)
[[nodiscard]] bool IsPerson(const Actor* apActor) noexcept;
// NemesisMan::Body
[[nodiscard]] uint8_t BodyOf(const Actor* apActor) noexcept;
// One of KARMA's bounty hunters (its leader keyword)
[[nodiscard]] bool IsKarmaHunter(const Actor* apActor) noexcept;

// "Fallout4.esm:3C": a form as its plugin and its id in it ("" if it isn't from a loaded plugin)
std::string KeyOf(uint32_t aFormId) noexcept;
// The worldspace the reference is in, as a key, "" inside
std::string WorldKeyOf(const TESObjectREFR* apRef) noexcept;
// The player's current location (BGSLocation) and its name, nullptr / "" for none
TESForm* PlayerLocation() noexcept;
std::string PlaceName() noexcept;
// The player is in a settlement (a location with LocTypeWorkshopSettlement)
[[nodiscard]] bool InSettlement() noexcept;
// A menu, a load, dialogue: nobody should be sent now
[[nodiscard]] bool IsBusy() noexcept;

// What a man is spawned as: his gang's encounter NPC for his rank (the game's legendary one when he is)
TESNPC* SpawnBase(const std::string& acGang, int aRank, bool aLegendary) noexcept;
// A spot aDistance from the player, on the ground, out of their sight if one is found (behind them first)
bool FindSpot(float aDistance, NiPoint3& aSpot) noexcept;
// The ground under a spot (its height changed), false if there's none to stand on
bool GroundAt(NiPoint3& aSpot) noexcept;
// ObjectReference.PlaceActorAtMe on the player, then set down at the spot facing the player. A temporary character:
// NemesisService deletes him once the player is far from him (Delete removes a temporary character at once), and
// CharacterService deletes any left in a save when the game connects.
Actor* Spawn(TESNPC* apBase, const NiPoint3& acSpot) noexcept;

// His name on his generated NPC (the crosshair's, the actor bars'); never on a shared base (a unique NPC's)
void SetName(Actor* apActor, const std::string& acName) noexcept;
// Rank 1-5: resistances; a limp slows him (NemesisMan::Injuries)
void ApplyRank(Actor* apActor, int aRank, uint8_t aInjuries) noexcept;
// KARMA's alignment on him (-2..2): its keyword and actor value, so KARMA's own kill rules count him
void SetAlignment(Actor* apActor, int aAlignment) noexcept;

// Health left (0..1) and crippled limbs (1 left leg, 2 right leg, 4 left arm, 8 right arm, 16 head)
[[nodiscard]] float HealthPercent(Actor* apActor) noexcept;
[[nodiscard]] uint8_t CrippledLimbs(Actor* apActor) noexcept;
// RequestNemesis::Cause by the weapon the killer holds
[[nodiscard]] uint8_t CauseOf(Actor* apKiller) noexcept;

// Through RD_Nemeses.esp's quest script, as the game's own holdup does it (holdupEffectScript): hands up (out of combat,
// unaggressive, the captive keyword that puts Interrogate on the crosshair), hands down as he was (nobody questioned
// him), or let go (he runs, or turns on the player again)
void Surrender(Actor* apActor) noexcept;
void Release(Actor* apActor) noexcept;
void LetGo(Actor* apActor, bool aFights) noexcept;
// Held up by the game's Intimidation perk (HoldupAV), and given the captive keyword for Interrogate
[[nodiscard]] bool IsHeldUp(Actor* apActor) noexcept;
void MarkCaptive(Actor* apActor) noexcept;
// The player's Charisma, for threatening a man
[[nodiscard]] int PlayerCharisma() noexcept;
// Interrogate was chosen on a captive since the last call (RD_Nemeses.esp's perk fragment set its global)
[[nodiscard]] bool TakeInterrogateRequest() noexcept;
// Stops fighting for good and walks off (no faction to be hostile with)
void Calm(Actor* apActor) noexcept;
void Attack(Actor* apActor, Actor* apTarget) noexcept;
// Actor.SetRestrained: they stay put and don't act, for someone riding on something
void Restrain(Actor* apActor, bool aRestrained) noexcept;
void Kill(Actor* apActor, Actor* apKiller) noexcept;
// The game's own walk somewhere: its travel package to the linked reference (Fallout4.esm BoS_TravelToLinkedRefWeaponDrawn,
// 00249038, its weapon out; RDVehicles.esp's RDV_RaiderCloseIn, the same at a jog, when that's loaded), apTarget set as their
// linked reference (ObjectReference.SetLinkedRef), the package put on them as the AI's created package. They path there
// on the navmesh as the game's AI does. False when it couldn't be put on them.
bool TravelTo(Actor* apActor, TESObjectREFR* apTarget) noexcept;
// The game's Aggression (Game.GetAggressionAV): 0, unaggressive, they never start a fight (they still fight back when attacked).
// Aggression returns -1 when it can't be read.
[[nodiscard]] float Aggression(Actor* apActor) noexcept;
void SetAggression(Actor* apActor, float aValue) noexcept;

// KARMA (KARMA.esm): the player's karma and alignment 0-4, false without KARMA; changes go through its ModKarma
bool ReadKarma(int& aKarma, int& aAlignment) noexcept;
void ModKarma(int aAmount, bool aSilent) noexcept;

// Caps: what the player has, and paying (false if short)
[[nodiscard]] int PlayerCaps() noexcept;
bool PayCaps(int aAmount) noexcept;

// A man's face (his generated NPC's race, sex and face as the game saves them) and putting it on another generated NPC.
// Only when the MCM's bKeepFaces is on (until seen working in game).
bool CaptureFace(Actor* apActor, std::string& aBuffer, uint32_t& aFlags) noexcept;
bool ApplyFace(Actor* apActor, const std::string& acBuffer, uint32_t aFlags) noexcept;

void ShowHud(const std::string& acText) noexcept;

// The wound log: his limbs' condition (head, torso, left arm, right arm, left leg, right leg; a hit takes from the one
// it lands on), and the name of what a character holds and its ammo ("fists" and "" bare-handed)
[[nodiscard]] std::array<float, 6> LimbConditions(Actor* apActor) noexcept;
[[nodiscard]] std::string WeaponName(Actor* apActor) noexcept;
[[nodiscard]] std::string AmmoName(Actor* apActor) noexcept;
// Gunpoint: the player's weapon is out and the middle of the view goes through him, this close at most
[[nodiscard]] bool AimedAt(Actor* apTarget, float aMaxDistance) noexcept;
[[nodiscard]] bool IsPipboyOpen() noexcept;
void ClosePipboy() noexcept;
// The player aims down the sights (the game activates nothing then)
[[nodiscard]] bool AimingDownSights() noexcept;
// The keyboard key the game's Activate is bound to in its control map (a Windows virtual key), 0 if unreadable
[[nodiscard]] int ActivateKey() noexcept;
} // namespace NemesisGame

#endif
