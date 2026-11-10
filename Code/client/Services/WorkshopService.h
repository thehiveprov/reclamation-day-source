#pragma once

#include <Games/Events.h>
#include <Games/Primitives.h>
#include <Misc/WorkshopEvents.h>
#include <Messages/NotifyCamps.h>
#include <Messages/NotifySettlementClaims.h>
#include <Messages/NotifySettlementProduction.h>
#include <Messages/NotifySettlementRatings.h>
#include <Messages/RequestCamp.h>
#include <Messages/NotifyWorkshopChange.h>
#include <Messages/RequestWorkshopChange.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct TESObjectREFR;

/**
 * @brief Shared settlements (Fallout 4): what a player builds, moves or scraps in build mode happens in every other
 * player's game too, at the same settlement. Objects built while connected get an item id from the server; objects
 * that come with the game or a plugin are known by their own id. Works for any workbench the game treats as a
 * workshop, and copies objects exactly as placed (position, rotation, scale) without checking build areas or
 * placement again, so mods that enlarge build areas or allow placing anywhere work as they do for the builder.
 *
 * Power: a wire is shared as the two objects it connects, and the other players connect them the way the game does
 * (SplineUtils, the settlement's power grid), so power flows the same everywhere. Objects that connect without wires
 * (conduits, connectors) connect when placed or moved; wires follow the objects they connect, and go with them.
 * Turrets are characters: the character sync brings them to the others, who only make them part of the settlement.
 *
 * Budget: what others build or scrap counts in the settlement's budget here as it did in their game (the game's own
 * add/take-off functions, run for it), so everyone's budget bar counts everyone's objects. Turrets built by others
 * aren't counted (their copies here come and go with the character sync).
 *
 * Claims (WorkshopClaims.cpp): while connected, a settlement is changed only by its leader (who claimed it at its
 * workbench) and the players they gave the right to build (place, move) or to scrap (scrap, store). Entering build
 * mode at a settlement nobody claimed asks to claim it; at one where the local player has no rights, build mode ends.
 * The build tools' hooks ask MayBuildHere / MayScrapHere. The overlay's Settlements tab shows the claims and lets
 * leaders give rights and abandon (settlements(request, payload), see OnUiRequest).
 *
 * C.A.M.P.s (WorkshopCamps.cpp, RD_Camps.esp): every player carries the C.A.M.P. device; using it from the Pip-Boy
 * sets their C.A.M.P. down in front of them and opens build mode at it, or, with one standing, closes the Pip-Boy and
 * asks in an overlay card: move it here (for caps), pick it up (kept as its layout, set down again free), or not now.
 * The Settlements tab picks it up too. Its module is a
 * settlement object of the C.A.M.P. like the rest (the server's Camp::kModId workshop ids), so it's built, kept and
 * removed in every game the same way; the C.A.M.P.'s objects wait for their module. Build area: a sphere of
 * Camp::kRadius around the module (the build tools hook the game's check).
 *
 * The settlements' day (WorkshopDays.cpp, ReclamationDay:Settlements): while a host is in the world only the host's
 * game runs the game's daily settlement update (settlers, production, happiness, attack rolls). The other games hold
 * theirs with the game's own deferral (WorkshopParent.DailyUpdateInProgress) and take the host's ratings of the claimed
 * settlements (the 45 actor values on the workbench the workshop menu and the Pip-Boy show). Production: host or leader
 * only. What the host's day puts in the workbench of a settlement led by another connected player is taken out there
 * and put in the leader's. Attacks happen in the host's game; the others see them through the character sync.
 */
class WorkshopService final : public BSTEventSink<Workshop::ItemPlacedEvent>,
                              public BSTEventSink<Workshop::ItemMovedEvent>,
                              public BSTEventSink<Workshop::ItemDestroyedEvent>,
                              public BSTEventSink<Workshop::WorkshopModeEvent>
{
public:
    WorkshopService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport);
    ~WorkshopService() noexcept override;

    TP_NOCOPYMOVE(WorkshopService);

    // Where an object is: position, rotation (radians), scale
    struct ObjectTransform
    {
        NiPoint3 Position;
        NiPoint3 Rotation;
        float Scale = 1.f;
    };

    // An object the local player scrapped, to build it again on undo
    struct ScrappedObject
    {
        uint32_t FormId = 0; // the reference scrapped: the world's own (from a plugin) is brought back itself
        uint32_t BaseId = 0;
        ObjectTransform Transform;
        uint32_t CellId = 0; // inside: its cell
        uint32_t WorldSpaceId = 0; // outside: its worldspace
        Vector<std::pair<uint32_t, uint32_t>> Components; // what scrapping it gave: form id, count
    };

    // How many of the local player's changes are kept per settlement for undo
    static constexpr size_t kMaxUndo = 5;

    // Whether the local player is in build mode, and at which workbench
    [[nodiscard]] bool IsInBuildMode() const noexcept { return m_inBuildMode; }
    [[nodiscard]] uint32_t GetBuildWorkshopId() const noexcept { return m_buildWorkshopId; }
    // The object outlined (or held) in build mode, null if none
    [[nodiscard]] static TESObjectREFR* GetOutlinedObject() noexcept;
    [[nodiscard]] static ObjectTransform GetTransform(const TESObjectREFR* apItem) noexcept;
    // Shares a settlement object the local player moved, rotated or scaled other than by grabbing it (the build tools)
    void ShareMove(const TESObjectREFR* apItem) noexcept;
    // A dead body (build tools) or a turret the local player is about to scrap: its scrap removes its character for
    // everyone
    void NoteScrappedCharacter(const TESObjectREFR* apActor) noexcept;

    // Undo: the local player's last kMaxUndo changes at each settlement (built, moved, scrapped), undone one by one,
    // newest first, in build mode at that settlement. Moving with the build tools: changes to the same object in a row
    // are one change.
    void RecordToolChange(const TESObjectREFR* apItem, const ObjectTransform& acBefore) noexcept;
    void RecordScrap(ScrappedObject aObject) noexcept;
    // What was undone (or why nothing was), to show the player
    [[nodiscard]] std::string UndoLastChange() noexcept;
    // While undoing a change: what the game does for it (scrapping what was built) isn't a change of its own
    [[nodiscard]] bool IsUndoing() const noexcept { return m_undoing; }

    // Claims: whether the local player may build (place, move) or scrap (scrap, store) at the settlement they're in
    // build mode at. Always outside build mode and when not connected. Cheap: the game asks for every object.
    [[nodiscard]] static bool MayBuildHere() noexcept;
    [[nodiscard]] static bool MayScrapHere() noexcept;
    // What the game refused for that, on the HUD (now and then, not for every frame it's refused)
    void ShowRefusal(bool aScrap) noexcept;
    // The overlay's Settlements tab: get, abandon <id>, release <id>, rights <id>:<build>:<scrap>:<name>, pickup <id>
    // (the local player's C.A.M.P.); the C.A.M.P. card: campMove, campPickUp, campCancel
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;
    // Is it the local player's own claim, or one of these players'? (Someone who isn't connected doesn't count)
    [[nodiscard]] bool IsClaimedBy(uint32_t aWorkshopFormId, const Vector<uint32_t>& acPlayerIds) const noexcept;
    // The claim box's answer (from the game's UI thread, through the runner)
    void OnClaimAnswer(uint32_t aWorkshopFormId, bool aClaim) noexcept;

    // C.A.M.P.s: the device and the module of RD_Camps.esp (0 while it isn't loaded)
    [[nodiscard]] static uint32_t GetCampDeviceId() noexcept;
    [[nodiscard]] static uint32_t GetCampModuleId() noexcept;
    // The local player used the C.A.M.P. device (the equip hook keeps the game from using it up)
    void OnCampDeviceUsed() noexcept;
    // The answer to "Move your C.A.M.P. here?" (pays and asks the server)
    void AnswerCampMove(bool aMove) noexcept;
    // Asks the server to pick the local player's C.A.M.P. up
    void PickUpCamp() noexcept;
    // A settlement's workbench as every game knows it (a C.A.M.P.'s by its id), false if it can't be shared
    bool GetSharedWorkshopId(uint32_t aWorkshopFormId, GameId& aId) const noexcept;
    // This game's workbench of a shared id, null if it isn't here (a C.A.M.P.'s module not built here yet)
    [[nodiscard]] TESObjectREFR* GetWorkshopRef(const GameId& acId) const noexcept;
    // The game's Yes/No box (MessageMenuManager), answered on the main loop (WorkshopClaims.cpp)
    static bool ShowYesNoBox(const std::string& acBody, const char* apYes, const char* apNo, std::function<void(bool)> aAnswer) noexcept;
    // Leaves build mode in the next frame (Workshop::RequestExitWorkshop)
    static void LeaveBuildMode() noexcept;

private:
    // A workshop event of the local player, queued by the game's event thread and sent from the next update
    struct LocalChange
    {
        uint8_t Type; // RequestWorkshopChange::Type
        uint32_t WorkshopFormId;
        uint32_t ItemFormId;
        bool Undoable = true; // false: an undo, or recorded already
    };

    struct UndoEntry
    {
        uint8_t Type = 0; // RequestWorkshopChange::Type
        uint32_t ItemFormId = 0; // built or moved: the object
        bool ToolChange = false; // moved with the build tools
        ObjectTransform Before; // moved: where it was
        ScrappedObject Scrapped;
    };

    BSTEventResult OnEvent(const Workshop::ItemPlacedEvent*, const EventDispatcher<Workshop::ItemPlacedEvent>*) override;
    BSTEventResult OnEvent(const Workshop::ItemMovedEvent*, const EventDispatcher<Workshop::ItemMovedEvent>*) override;
    BSTEventResult OnEvent(const Workshop::ItemDestroyedEvent*, const EventDispatcher<Workshop::ItemDestroyedEvent>*) override;
    BSTEventResult OnEvent(const Workshop::WorkshopModeEvent*, const EventDispatcher<Workshop::WorkshopModeEvent>*) override;

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnNotifyWorkshopChange(const NotifyWorkshopChange&) noexcept;

    void RegisterEvents() noexcept;
    void QueueLocalChange(uint8_t aType, const TESObjectREFR* apWorkshop, const TESObjectREFR* apItem) noexcept;
    // False while the change can't be shared yet, to try again (a wire not connected yet, a turret its character sync
    // hasn't picked up yet)
    bool SendLocalChange(const LocalChange& acChange) noexcept;
    // A wire's two ends, for the others (false: not connected yet)
    bool FillWireEnds(const TESObjectREFR* apWire, RequestWorkshopChange& aRequest) const noexcept;

    // Undo
    void RecordChange(const LocalChange& acChange) noexcept;
    void PushUndo(uint32_t aWorkshopFormId, UndoEntry aEntry) noexcept;
    void TrackOutlined() noexcept;
    std::string UndoBuilt(const UndoEntry& acEntry) noexcept;
    std::string UndoMoved(const UndoEntry& acEntry) noexcept;
    std::string UndoScrapped(const UndoEntry& acEntry, TESObjectREFR* apWorkshop) noexcept;

    // Applies another player's change here. False while what it changes isn't loaded here, to try again later.
    bool ApplyChange(const NotifyWorkshopChange& acMessage) noexcept;
    bool ApplyPlaced(const NotifyWorkshopChange& acMessage) noexcept;
    // Connects the wire's two objects here, as the game does when a player wires them
    bool ApplyWire(const NotifyWorkshopChange& acMessage, TESForm* apBase, TESObjectREFR* apWorkshop) noexcept;
    // A turret: the character sync brings it, it becomes part of the settlement here
    bool ApplyCharacterPlaced(const NotifyWorkshopChange& acMessage, TESObjectREFR* apWorkshop) noexcept;
    // This client's copy of one of a wire's two objects, null if it isn't here (yet)
    [[nodiscard]] TESObjectREFR* FindWireEnd(const NotifyWorkshopChange& acMessage, size_t aEnd) const noexcept;
    // After another player moved an object: its wires follow, it connects to what it now touches
    static void UpdatePower(TESObjectREFR* apItem, TESObjectREFR* apWorkshop) noexcept;
    // Before another player's scrap takes an object away here: its wires (or itself, a wire) are disconnected first
    void DisconnectWires(TESObjectREFR* apItem, TESObjectREFR* apWorkshop) noexcept;
    // Shared budget: an object built here for another player counts in the settlement's budget once its 3D is loaded
    // (the cost comes from it), one scrapped here for another player comes off it, as in their game
    void QueueBudget(const TESObjectREFR* apItem, const TESObjectREFR* apWorkshop) noexcept;
    // True if the object was still waiting to be counted (then it never was)
    bool ForgetPendingBudget(uint32_t aItemFormId) noexcept;
    void RunPendingBudget() noexcept;
    void ApplyBudget(TESObjectREFR* apItem, TESObjectREFR* apWorkshop, bool aAdd) noexcept;
    // This client's copy of the object a change is about, null if it isn't loaded (or doesn't exist) here
    [[nodiscard]] TESObjectREFR* FindChangedRef(const NotifyWorkshopChange& acMessage) const noexcept;
    // Whether the spot is in the part of the world this client has loaded
    [[nodiscard]] bool IsLoadedHere(const NotifyWorkshopChange& acMessage) const noexcept;
    // Synced characters: server id of a reference here, and the reference here of a server id (0 if none)
    [[nodiscard]] uint32_t FindCharacterServerId(uint32_t aFormId) const noexcept;
    [[nodiscard]] uint32_t FindCharacterFormId(uint32_t aServerId) const noexcept;
    void RunPendingChanges() noexcept;

    // Which object here each item is, remembered per world (settlement-items.txt in its save folder) so that after the
    // server restarts, or in a later session, the objects this game has are used instead of built again, and those
    // moved or scrapped while this player was away follow
    void MapItem(uint32_t aItemId, uint32_t aFormId) noexcept;
    void UnmapItem(uint32_t aItemId) noexcept;
    // The object here of an item, null if it isn't loaded or isn't that object anymore (its base changed)
    [[nodiscard]] TESObjectREFR* GetMappedRef(uint32_t aItemId) const noexcept;
    // This game has the item's object, in a cell that isn't loaded: changes to it wait for it
    [[nodiscard]] bool IsRememberedElsewhere(const NotifyWorkshopChange& acMessage) const noexcept;
    void LoadItemMap() noexcept;
    void SaveItemMap() noexcept;

    // Claims (WorkshopClaims.cpp)
    void OnNotifySettlementClaims(const NotifySettlementClaims&) noexcept;
    // The local player's rights at their build-mode settlement, from the claims: asks to claim it, or ends build mode.
    // Entered: just entered build mode (the rights are said on the HUD then, and when they change).
    void CheckBuildModeClaim(bool aEntered) noexcept;
    void AskToClaim(TESObjectREFR* apWorkshop) noexcept;
    [[nodiscard]] const SettlementClaim* FindClaim(uint32_t aWorkshopFormId) const noexcept;
    // kMayBuild | kMayScrap at a workbench, from m_rightsByWorkshop (any thread)
    [[nodiscard]] uint8_t GetRightsAt(uint32_t aWorkshopFormId) noexcept;
    void SendClaimRequest(uint8_t aAction, const GameId& acWorkshopId, const String& acSettlementName = {}, const String& acPlayerName = {}, bool aBuild = false,
                          bool aScrap = false) noexcept;
    void SendClaimsUiState() const noexcept;
    void ResetClaims() noexcept;
    // The rights table from the claims and C.A.M.P.s (again when a C.A.M.P.'s module is built here)
    void RefreshRights() noexcept;

    // The settlements' day (WorkshopDays.cpp)
    // Each update: holds this game's day while the host's runs it; the host's game sends its ratings and passes
    // production on to the settlements' leaders
    void RunSettlementDay() noexcept;
    void HoldDay(bool aHold) noexcept;
    void SendRatings(std::chrono::steady_clock::time_point aNow) noexcept;
    void OnNotifySettlementRatings(const NotifySettlementRatings&) noexcept;
    // False while the workbench isn't here (a C.A.M.P.'s module not built yet), to try again
    bool ApplyRatings(const SettlementRatings& acRatings) noexcept;
    void RunProduction(std::chrono::steady_clock::time_point aNow) noexcept;
    void OnNotifySettlementProduction(const NotifySettlementProduction&) noexcept;
    bool GiveProduction(const NotifySettlementProduction& acMessage) noexcept;
    void ResetSettlementDay() noexcept;

    // C.A.M.P.s (WorkshopCamps.cpp)
    void OnNotifyCamps(const NotifyCamps&) noexcept;
    // Each update: the device in the player's inventory, build mode at a C.A.M.P. just set down
    void RunCamps() noexcept;
    [[nodiscard]] const Camp* FindLocalCamp() const noexcept;
    // Why a C.A.M.P. can't be set down at a spot, empty if it can (the server checks other C.A.M.P.s)
    [[nodiscard]] std::string WhyNoCampHere(const NiPoint3& acSpot) const noexcept;
    // The C.A.M.P. card (move here, pick up, not now) in the overlay, with the overlay's cursor
    void ShowCampCard() noexcept;
    void CloseCampCard() noexcept;
    // After the local player's C.A.M.P. moved or was set down: each object built at it here is checked once its spot
    // is loaded (QueueFitCheck from ApplyPlaced); one that ended up entirely underground goes into the C.A.M.P.'s
    // workbench as the materials it's built from (RunFitChecks, StoreAsMaterials)
    void QueueFitCheck(const NotifyWorkshopChange& acMessage, const TESObjectREFR* apRef) noexcept;
    void RunFitChecks() noexcept;
    bool StoreAsMaterials(TESObjectREFR* apRef, TESObjectREFR* apWorkshop) noexcept;

    World& m_world;
    TransportService& m_transport;

    bool m_eventsRegistered = false;
    std::mutex m_localChangesLock;
    Vector<LocalChange> m_localChanges;
    // Local changes that couldn't be shared yet, tried again until then
    struct DeferredChange
    {
        LocalChange Change;
        std::chrono::steady_clock::time_point GiveUpAt;
    };
    Vector<DeferredChange> m_deferredChanges;
    bool m_inBuildMode = false; // the local player, set from the game's event thread
    uint32_t m_buildWorkshopId = 0;

    TiltedPhoques::Map<uint32_t, uint32_t> m_itemRefs; // item id -> reference form id here
    TiltedPhoques::Map<uint32_t, uint32_t> m_refItems; // reference form id -> item id
    TiltedPhoques::Map<uint32_t, uint32_t> m_itemBases; // item id -> base form id of its object here
    bool m_itemMapLoaded = false;
    bool m_itemMapDirty = false;
    std::filesystem::path m_itemMapFile;
    std::chrono::steady_clock::time_point m_itemMapSavedAt{};
    TiltedPhoques::Map<uint32_t, uint32_t> m_scrappedBodies; // reference form id -> character server id (m_localChangesLock)
    // Changes waiting for their spot to load here, by object (see PendingKey)
    TiltedPhoques::Map<uint64_t, NotifyWorkshopChange> m_pendingChanges;
    struct PendingBudget
    {
        uint32_t ItemFormId;
        uint32_t WorkshopFormId;
        std::chrono::steady_clock::time_point GiveUpAt;
    };
    Vector<PendingBudget> m_pendingBudget;
    std::chrono::steady_clock::time_point m_lastPendingRun{};

    std::mutex m_undoLock;
    TiltedPhoques::Map<uint32_t, Vector<UndoEntry>> m_undo; // by workbench form id, oldest first
    bool m_undoing = false;
    // The outlined object and where it was when outlined: where an object the player moves was before
    uint32_t m_outlinedId = 0;
    ObjectTransform m_outlinedBefore;

    // Claims, as the server last sent them
    Vector<SettlementClaim> m_claims;
    // The local player's rights at each claimed settlement (workbench form id), for the build mode event: unclaimed
    // ones have none
    std::mutex m_rightsLock;
    TiltedPhoques::Map<uint32_t, uint8_t> m_rightsByWorkshop;
    String m_localName; // ours, as the server knows it
    bool m_isAdmin = false;
    // Build mode was entered (game event thread): check the claim from the next update
    std::atomic<bool> m_claimCheckPending{false};
    uint32_t m_askedWorkshopId = 0; // the claim box is up for it
    std::chrono::steady_clock::time_point m_refusalShownAt{};
    // MayBuildHere / MayScrapHere: kMayBuild | kMayScrap for the build-mode settlement, read from the game's threads
    static constexpr uint8_t kMayBuild = 1;
    static constexpr uint8_t kMayScrap = 2;
    static inline std::atomic<uint8_t> s_buildRights{kMayBuild | kMayScrap};
    static inline std::atomic<bool> s_inBuildMode{false};

    // C.A.M.P.s, as the server last sent them
    Vector<Camp> m_camps;
    // Set down just now: build mode at it once its module is built here (given up at that time)
    std::optional<std::chrono::steady_clock::time_point> m_enterCampUntil;
    std::chrono::steady_clock::time_point m_campDeviceCheckedAt{};
    // Asked to move it here (free)
    std::optional<RequestCamp> m_pendingCampMove;
    // Why it can't move to that spot (empty if it can): the card offers only picking up then
    std::string m_campMoveRefusal;
    // The card: wanted once the Pip-Boy is down (the device is used from it), open
    bool m_campCardWanted = false;
    bool m_campCardOpen = false;
    // The local player's C.A.M.P. was stored when the server last said: told on the HUD when that changes
    bool m_localCampStored = false;
    // Where the local player's C.A.M.P. stood when the server last said (none: no C.A.M.P. or stored)
    std::optional<std::array<float, 3>> m_localCampSpot;
    // Its objects being checked after it moved or was set down: until when, which ones (item id, when), and how many
    // went into its workbench so far (said on the HUD once the checks are over)
    uint32_t m_fitCampId = 0;
    std::chrono::steady_clock::time_point m_fitUntil{};
    Vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>> m_fitChecks;
    int m_fitStored = 0;

    // The settlements' day: who runs it (the host's player id, 0: every game its own), this game's held
    uint32_t m_dayHostId = 0;
    bool m_dayHeld = false;
    std::chrono::steady_clock::time_point m_dayHeldAt{};
    // The host's game: told the server it runs the day (this connection), when ratings were last looked at and all sent,
    // what was sent of each (by workbench, see DayKey)
    bool m_daySentHello = false;
    std::chrono::steady_clock::time_point m_ratingsSentAt{};
    std::chrono::steady_clock::time_point m_ratingsFullAt{};
    TiltedPhoques::Map<uint64_t, Vector<float>> m_sentRatings;
    // The others: ratings for workbenches not here yet
    TiltedPhoques::Map<uint64_t, SettlementRatings> m_pendingRatings;
    // The host's game: what each workbench led by someone else held at the last look (by workbench form id, then
    // object form id), when it was looked at, until when the host's own use of one keeps it from counting
    TiltedPhoques::Map<uint32_t, TiltedPhoques::Map<uint32_t, int32_t>> m_productionWatch;
    std::chrono::steady_clock::time_point m_productionCheckedAt{};
    std::chrono::steady_clock::time_point m_productionBusyUntil{};
    // The leader's game: production for workbenches not here yet
    Vector<NotifySettlementProduction> m_pendingProduction;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_settlementRatingsConnection;
    entt::scoped_connection m_settlementProductionConnection;
    entt::scoped_connection m_workshopChangeConnection;
    entt::scoped_connection m_settlementClaimsConnection;
    entt::scoped_connection m_campsConnection;
};
