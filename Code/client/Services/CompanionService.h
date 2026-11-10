#pragma once

#include <Events/EventDispatcher.h>

struct World;
struct Actor;
struct TESObjectREFR;
struct TESNPC;
struct BGSKeyword;
struct UpdateEvent;

/**
 * @brief Adoptable dogs (RD_Companions.esp): each player can adopt one of the dogs placed around the Commonwealth. It
 * becomes their Dogmeat, in the Dogmeat slot of the Followers quest of their own game, so his talk, commands, tricks,
 * Find stuff and dog armor all work with it. Fallout 4 only.
 *
 * Vanilla allows a dog or a human companion, not both: FollowersScript's SetDogmeatCompanion dismisses the human and
 * SetCompanion dismisses the dog. An adopted dog travels beside a human companion instead. Its record has Dogmeat's
 * WorkshopNPCScript but not his DogmeatActorScript, and DismissDogmeatCompanion does nothing for a dog without it, so no
 * vanilla script sends it away. The joining and leaving FollowersScript would do are done here, minus the dismissals.
 *
 * His dialogue only accepts Dogmeat himself (GetIsID Dogmeat): the condition is answered yes for the dog in the local
 * player's own Dogmeat slot, and only that one, so talking to any other dog never commands yours or the others.
 *
 * Pressing E on a dog nobody adopted adopts it (on your own it's his talk). His "go home" line (DogmeatQuest stage 90)
 * opens the settlement list like his: the dog goes to the settlement picked, or back to the spot it was adopted from.
 * On adopting a dog, and when vanilla Dogmeat joins, the overlay asks for its name. Names are kept per character in
 * the save folder (companion-names.txt) and put on the dog's record.
 *
 * All of it is off while RD_Companions.esp isn't loaded: the game then plays as vanilla, Dogmeat included.
 */
class CompanionService final : public BSTEventSink<TESQuestStageEvent>
{
public:
    CompanionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CompanionService() noexcept = default;

    TP_NOCOPYMOVE(CompanionService);

    // From the activation hook (the game's thread). True when the activation was handled here and the game must not
    // run it.
    bool OnActivate(TESObjectREFR* apObject, Actor* apActivator) noexcept;

    // The local player's adopted dog, nullptr if they have none (or vanilla Dogmeat is with them)
    [[nodiscard]] Actor* GetDog() const noexcept;
    [[nodiscard]] bool IsAdoptable(const TESObjectREFR* apObject) const noexcept;
    // The dog in the Followers' Dogmeat slot, adopted or vanilla Dogmeat
    [[nodiscard]] Actor* GetDogmeatSlotActor() const noexcept;

    bool Adopt(Actor* apDog) noexcept;
    // aToSettlement: the settlement list first, as his "go home" does
    void Release(bool aToSettlement) noexcept;

    // From the overlay: companions(request, payload), request "name" ("baseId:name", baseId in hex) or "keep" ("baseId")
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;

    BSTEventResult OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*) override;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    [[nodiscard]] BGSKeyword* GetAdoptableKeyword() const noexcept;
    // Back to a stray: teammate off, out of the slot when it's still in it
    void Unfollow(Actor* apDog, bool aClearSlot) noexcept;

    void AskForName(TESNPC* apNpc) noexcept;
    void CloseNamePrompt() noexcept;
    // "plugin:localId", how a dog's name is kept (form IDs change with the load order); empty if its plugin isn't known
    [[nodiscard]] std::string KeyOf(const TESNPC* apNpc) const noexcept;
    void LoadNames() noexcept;
    void SaveNames() const noexcept;
    void ApplyNames() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;

    double m_nextCheck = 0.0;
    uint32_t m_slotActorId = 0;
    uint32_t m_promptNpcId = 0;
    std::string m_promptGreeting;

    // Per character, see LoadNames
    std::filesystem::path m_namesFile;
    std::string m_character;
    // "plugin:localId" (local ID in hex) -> name
    std::map<std::string, std::string> m_names;
};
