#pragma once

#include <Events/PacketEvent.h>
#include <Messages/NotifyWorkshopChange.h>
#include <Structs/Camp.h>
#include <Structs/SettlementClaim.h>
#include <Structs/SettlementRatings.h>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct RequestWorkshopChange;
struct RequestSettlementClaim;
struct RequestCamp;
struct RequestSettlementRatings;
struct RequestSettlementProduction;

/**
 * @brief Shared settlements: relays what players build, move and scrap at settlement workbenches, and keeps the
 * settlements' current state for players joining later.
 *
 * Kept across server restarts in settlements.txt, in the world's folder (RD_WORLD_DATA, set by the game that hosts the
 * world) or in data\ next to a dedicated server: what was built (with the same item ids, the players' games remember
 * which of their objects is which item), what happened to the game's own objects, and what was scrapped of what was
 * built, for players who weren't there to see it. Written a moment after each change (the host's game ends its server
 * without warning), through a temporary file so a cut-off write never loses what was there.
 *
 * Claims: the first player to claim a settlement at its workbench leads it and gives the players of their party the
 * right to build (place, move) and to scrap (scrap, store) there; nobody changes a settlement nobody claimed. Players
 * are known by name, rights stay theirs while the leader is away. The leader can abandon it, the host (or an admin)
 * can release anyone's. Kept in settlement-claims.txt next to settlements.txt.
 *
 * C.A.M.P.s (see Camp): one per player, set down outside settlements. Its module is a settlement object of the
 * C.A.M.P. (workshop id GameId{Camp::kModId, id}), so every game builds, keeps and removes it as the rest; its owner
 * leads it (a claim made with it, which can't be abandoned). Moving one takes every object of it away and builds it
 * again at the new spot, turned and shifted as the module is, under new item ids: games that have the old objects in
 * cells they haven't loaded remove them when they do, as for any scrap. Turrets don't move (they're characters): they
 * go. Picking one up takes everything away the same way and keeps it as the C.A.M.P.'s layout; setting it down again
 * (free) builds the layout at the new spot as a move would. Kept in camps.txt next to settlements.txt, layouts too.
 *
 * The settlements' day (WorkshopDays.cpp): while the host is connected only their game runs it (the others hold theirs,
 * see the client's WorkshopDays.cpp). The host sends the ratings of the claimed settlements, kept here for players who
 * join and relayed to the others; the day's production at a settlement whose leader isn't the host is handed to that
 * leader. Nothing of it is saved: it's the host's game's, and goes with the host when they leave.
 */
class WorkshopService
{
public:
    WorkshopService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    void OnWorkshopChange(const PacketEvent<RequestWorkshopChange>&) noexcept;
    void OnSettlementClaim(const PacketEvent<RequestSettlementClaim>&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;

    // The settlements' day (WorkshopDays.cpp)
    void OnSettlementRatings(const PacketEvent<RequestSettlementRatings>&) noexcept;
    void OnSettlementProduction(const PacketEvent<RequestSettlementProduction>&) noexcept;
    // Who runs the day and every rating kept, to one player or everyone but the host
    void SendRatings(Player* apPlayer = nullptr) const noexcept;
    void OnDayPlayerLeave(const Player& acPlayer) noexcept;

    void Load() noexcept;
    void Save() noexcept;

    // Why a player may not change a settlement (build: place, move; scrap: scrap, store), null if they may
    [[nodiscard]] const char* WhyNotAllowed(const Player& acPlayer, const GameId& acWorkshopId, bool aScrap) const noexcept;
    // Every claim, with whether each leader is connected (apLeaving is on its way out), to one player or everyone
    void SendClaims(Player* apPlayer = nullptr, const Player* apLeaving = nullptr) const noexcept;
    void LoadClaims() noexcept;
    void SaveClaims() const noexcept;

    // C.A.M.P.s
    void OnCamp(const PacketEvent<RequestCamp>&) noexcept;
    // Why a C.A.M.P. can't be at a spot (another one too close), null if it can; acIgnored: the one being moved
    [[nodiscard]] const char* WhyNoCampHere(const GameId& acWorldSpaceId, const std::array<float, 3>& acPosition, uint32_t aIgnoredCampId) const noexcept;
    // A new settlement object, kept and sent to everyone (from the server: a C.A.M.P.'s module, or a moved object)
    uint32_t AddItem(NotifyWorkshopChange aItem) noexcept;
    // A settlement object scrapped, kept as such for who still has it and sent to everyone; a turret's character goes
    void RemoveItem(uint32_t aItemId, uint32_t aPlayerId) noexcept;
    void TrimRemoved() noexcept;
    // A C.A.M.P. and everything built at it goes, with its claim
    void RemoveCamp(uint32_t aCampId, uint32_t aPlayerId) noexcept;
    // Its objects as built now, in the order they're built again: the module, objects, wires (between objects)
    [[nodiscard]] Vector<NotifyWorkshopChange> GetCampObjects(const Camp& acCamp) const noexcept;
    // Builds a C.A.M.P.'s objects again at a new spot, turned and shifted as its module is from where they stood (the
    // C.A.M.P.'s Position and Heading), under new item ids, the module (aModuleItemId among them) first; the C.A.M.P.
    // takes the spot and the new module. Turrets are left out. The new item ids, and how many turrets were left out.
    std::pair<Vector<uint32_t>, size_t> RebuildCamp(Camp& aCamp, const Vector<NotifyWorkshopChange>& acObjects, uint32_t aModuleItemId, const GameId& acWorldSpaceId,
                                                    const std::array<float, 3>& acPosition, float aHeading, uint32_t aPlayerId) noexcept;
    // Its owner picked it up: everything built at it goes, kept as its layout
    void PickUpCamp(Camp& aCamp, uint32_t aPlayerId) noexcept;
    void SendCamps(Player* apPlayer = nullptr) const noexcept;
    void LoadCamps() noexcept;
    void SaveCamps() const noexcept;

    // Objects built while connected, by item id, as last placed or moved
    TiltedPhoques::Map<uint32_t, NotifyWorkshopChange> m_items;
    uint32_t m_nextItemId = 1;

    // Objects that come with the game or plugins (GameId as ModId << 32 | BaseId) that were moved or scrapped: the
    // last change of each
    TiltedPhoques::Map<uint64_t, NotifyWorkshopChange> m_refChanges;

    // Built objects scrapped since, by item id (their last state, as a removal): players who had them remove theirs
    TiltedPhoques::Map<uint32_t, NotifyWorkshopChange> m_removed;

    std::filesystem::path m_dataFile;
    bool m_dirty = false;
    std::chrono::steady_clock::time_point m_savedAt{};

    // Claimed settlements, by workbench (as m_refChanges); LeaderPlayerId is filled in when sent
    TiltedPhoques::Map<uint64_t, SettlementClaim> m_claims;
    std::filesystem::path m_claimsFile;

    // C.A.M.P.s by id
    TiltedPhoques::Map<uint32_t, Camp> m_camps;
    // Stored C.A.M.P.s' objects as they stood when picked up (their old item ids, wires' ends among them), by C.A.M.P.
    struct CampLayout
    {
        uint32_t ModuleItemId{};
        Vector<NotifyWorkshopChange> Objects;
    };
    TiltedPhoques::Map<uint32_t, CampLayout> m_layouts;
    uint32_t m_nextCampId = 1;
    std::filesystem::path m_campsFile;

    // The settlements' day: the host's player id (0: no host, every game runs its own), their ratings by workbench
    uint32_t m_dayHostId = 0;
    TiltedPhoques::Map<uint64_t, SettlementRatings> m_ratings;

    World& m_world;

    entt::scoped_connection m_workshopChangeConnection;
    entt::scoped_connection m_settlementRatingsConnection;
    entt::scoped_connection m_settlementProductionConnection;
    entt::scoped_connection m_settlementClaimConnection;
    entt::scoped_connection m_campConnection;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_updateConnection;
};
