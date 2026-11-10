#pragma once

#include <Structs/FactionStanding.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyFactionStanding;
struct NotifySharedQuest;
struct Actor;
struct TESQuest;
struct BGSBaseAlias;

/**
 * @brief Factions (Fallout 4, docs/FACTIONS.md): standing and reactivity.
 *
 * Standing: which of the Commonwealth's factions the local player is in, was put out of, is fought by and is allied
 * with, read from this game every 2 s (the globals the game's quests set, membership through Actor.IsInFaction, and each
 * faction's fight reaction to PlayerFaction through GetGroupFightReaction, as the game's own reaction between two
 * actors reads it), sent when it changes and on connecting. The others' standings are kept by player id.
 *
 * Reactivity, rules the game lacks, done the game's own way: a Minuteman who joins the Gunners (a mod's faction: the
 * game has no way to join them) or the raiders (the Nuka-World gangs, or a mod putting the player in RaiderFaction) is
 * abandoned by the Minutemen (ReclamationDay:Factions.AbandonMinutemen: the sequence the game's own Brotherhood and
 * Railroad kick-outs run). Offline too: it's a rule of the world.
 *
 * Other players' characters carry their own standing in this game. The game decides how two actors react from all
 * their factions' relations, and an ally or friend relation beats an enemy one (the reaction between two actors,
 * 339469). So each copy is put in RD_Social.esp's standing factions for its owner's standing: RD_FoughtBy<faction>
 * when that faction fights its owner, RD_AlliedWith<faction> when it's their ally or they're in it; and those factions
 * are made the faction's enemies and allies in this game (Faction.SetEnemy / SetAlly). Copies are in PlayerFaction
 * (Actor::SetPlayerRespawnMode: what's friendly to the local player is friendly to them); where this game's
 * PlayerFaction is friendly with a faction that fights the copy's owner, the copy is in PlayerAllyFaction instead, so
 * it stays the local player's ally without that faction's friendship winning.
 *
 * Faction quests together (FactionQuests.cpp): when one of a faction's radiant quests starts here (the lists read from
 * the plugins: the Minutemen's settlement and recruitment quests, the Brotherhood's, Railroad's and Institute's
 * repeatable ones, Nuka-World's raids), the local player is in that faction and party members are too, its aliases'
 * fills are sent (RequestSharedQuest) and those members are invited to do it together (QuestService). In their games
 * the quest is started the game's own way with the same fills: its aliases are made Forced to the starter's references
 * and locations as that game has them (BGSBaseAlias fill type +0x20 = 1, fill data +0x28: a reference's handle, a
 * location's form, as the game's own alias loaders keep them), Quest.Start fills them (ReclamationDay:Factions), the
 * aliases are put back once it has started, and the invite is accepted so their stages go together.
 *
 * Characters are the starter's: what the quest made or found that exists only in the starter's game (the enemies it
 * spawned, a settler, those in a reference collection alias: attackers, defenders) is held empty in the others' games
 * (the alias Forced to no reference and made Optional for the start, so their game neither makes nor finds its own),
 * the starter's game sends each one's server id once the character sync has it (an Update), and the others put their
 * copy of that character in the alias (ReferenceAlias.ForceRefTo, RefCollectionAlias.AddRef) once it's in their game.
 * Objects the quest makes (an item to fetch, a marker) are each game's own, as loot is: made by its own start at the
 * same place, the alias they're made at being the starter's.
 */
class FactionService final : public BSTEventSink<TESQuestStartStopEvent>
{
public:
    FactionService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~FactionService() noexcept override = default;

    TP_NOCOPYMOVE(FactionService);

    [[nodiscard]] const FactionStanding& GetLocalStanding() const noexcept { return m_local; }
    // Another player's standing, null if their game hasn't said yet
    [[nodiscard]] const FactionStanding* GetStanding(uint32_t aPlayerId) const noexcept;
    // Puts another player's character in the standing factions of its owner's standing (also after the character sync
    // set its factions again, Actor::SetFactions)
    void ApplyStanding(Actor* apCopy) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnNotifyStanding(const NotifyFactionStanding& acMessage) noexcept;

    // This game's standing now; false while there's no player to read it from (main menu, loading)
    [[nodiscard]] bool ReadLocalStanding(FactionStanding& aStanding) const noexcept;
    // The abandon rule, once the standing calls for it
    void RunReactivity() noexcept;
    // The standing factions' relations in this game (once RD_Social.esp's factions and the natives are there)
    bool SetUpStandingRelations() noexcept;
    void ApplyStanding(Actor* apCopy, const FactionStanding& acStanding) noexcept;
    [[nodiscard]] uint32_t FindPlayerId(const Actor* apCopy) const noexcept;

    // Faction quests together (FactionQuests.cpp)
    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    // A quest started here (main thread): a radiant quest the local player got, or one started for a party member
    void OnQuestStarted(uint32_t aQuestId) noexcept;
    void ShareQuest(TESQuest* apQuest, FactionStanding::Faction aFaction) noexcept;
    void OnNotifySharedQuest(const NotifySharedQuest& acMessage) noexcept;
    // Its aliases back as the plugin has them, once started (or given up on)
    void RestoreAliases(uint32_t aQuestId) noexcept;
    // Each second: the starter sends characters synced since; the others put the starter's characters in their aliases
    void RunSharedFills(std::chrono::steady_clock::time_point aNow) noexcept;
    // Faction of a radiant quest (FactionStanding::kCount if it isn't one)
    [[nodiscard]] FactionStanding::Faction RadiantFaction(uint32_t aQuestId) noexcept;
    // Synced characters: server id of a reference here, and the reference here of a server id (0 if none)
    [[nodiscard]] uint32_t FindServerId(uint32_t aFormId) const noexcept;
    [[nodiscard]] uint32_t FindFormId(uint32_t aServerId) const noexcept;

    World& m_world;
    TransportService& m_transport;

    FactionStanding m_local{};
    bool m_localRead = false;
    bool m_sent = false;
    bool m_relationsSet = false;
    std::chrono::steady_clock::time_point m_readAt{};
    // The abandoning was asked for (it runs on the VM's time): not asked again until the standing shows it
    std::chrono::steady_clock::time_point m_abandonAskedAt{};
    TiltedPhoques::Map<uint32_t, FactionStanding> m_standings; // others', by player id

    // Radiant quests by form id here, and their faction (filled once the plugins are loaded)
    TiltedPhoques::Map<uint32_t, FactionStanding::Faction> m_radiants;
    bool m_radiantsRead = false;
    bool m_questSinkRegistered = false;
    // Quests being started here for a party member: who, what their aliases were before (flags, fill type and fill
    // data, BGSBaseAlias + 0x1C to + 0x40), until when to wait for the start
    struct Mirror
    {
        uint32_t StarterId{};
        Vector<std::pair<BGSBaseAlias*, std::array<uint8_t, 0x24>>> Saved;
        std::chrono::steady_clock::time_point GiveUpAt{};
    };
    std::mutex m_mirrorsLock;
    TiltedPhoques::Map<uint32_t, Mirror> m_mirrors;
    // The starter's game: the characters of a shared quest the character sync hadn't picked up when it was shared
    struct Watched
    {
        uint32_t AliasId{};
        uint8_t Type{}; // SharedQuest::Fill::Kind
        uint32_t FormId{}; // the character here
    };
    struct Sharing
    {
        GameId QuestId{};
        uint8_t Faction{};
        Vector<Watched> Pending;
        std::chrono::steady_clock::time_point Until{};
    };
    TiltedPhoques::Map<uint32_t, Sharing> m_sharing; // by quest form id
    // The others' games: the starter's characters still to be put in their aliases here
    struct Waiting
    {
        uint32_t AliasId{};
        uint8_t Type{};
        uint32_t ServerId{}; // 0 until the starter's Update says
        GameId FormId{};     // the world's own (a collection entry)
    };
    TiltedPhoques::Map<uint32_t, Vector<Waiting>> m_waiting; // by quest form id
    std::chrono::steady_clock::time_point m_fillsCheckedAt{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_standingConnection;
    entt::scoped_connection m_sharedQuestConnection;
};
