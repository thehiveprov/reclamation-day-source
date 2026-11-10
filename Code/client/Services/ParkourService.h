#pragma once

#include <ParkourMoves.h>
#include <Games/Primitives.h>

#include <array>
#include <unordered_map>

struct World;
struct UpdateEvent;
struct PlayerCharacter;
struct Actor;
struct FileSound;
struct TESIdleForm;
struct NotifyMovementAction;

/**
 * @brief ZK's Fluid Movement: jump at a ledge to climb it (four heights), step up onto something low, vault over an
 * obstacle while running, or keep jump held in the air to grab a ledge - and the same moves played by the other
 * players' characters here. Our own rewrite for Fallout 4 of the parkour of SkyParkour V3 by Tsptds (Skyrim, Nexus mod
 * 132292), with its ledge rules and its animations retargeted to Fallout 4's skeletons (ParkourMoves.h,
 * Tools/ConvertParkourAnimations). As in the original, the character's collision is off for the length of a move and
 * it travels the way the animation does, eased onto the ledge it found. Human NPCs make the moves too, where something
 * is in their way (docs/NPC_PARKOUR.md). Fallout 4 only.
 */
struct ParkourService
{
    // Set by MovementService from RD's MCM page Movement: on a hosted world the host's apply to everyone, except the
    // markers (each player's own)
    struct Rules
    {
        bool Parkour{true};
        // share of the maximum AP a climb as high as the head or higher takes (others take half); without it, the
        // climb fails (the struggling animation)
        float ApCost{0.15f};
        // a marker in the HUD on the ledge or obstacle a jump would take
        bool Markers{true};
        // crouch pressed while falling, close above the ground: a roll on landing (half the AP of a climb)
        bool LandingRoll{true};
        // human NPCs climb and vault what's in their way
        bool NpcParkour{true};
        // human NPCs in a fight slide into cover and at their target (docs/ENEMY_TACTICS.md)
        bool NpcSlides{true};
    };

    ParkourService(World& aWorld, entt::dispatcher& aDispatcher);
    ~ParkourService() noexcept;

    TP_NOCOPYMOVE(ParkourService);

    [[nodiscard]] const Rules& GetRules() const noexcept { return m_rules; }
    void SetRules(const Rules& acRules) noexcept { m_rules = acRules; }

    // From the hook on the game's jump control (main thread): true when the press started a move, which then takes
    // the place of the jump
    bool OnJumpButton(bool aDown, bool aJustPressed) noexcept;
    // From the hook on the game's sneak control (main thread): true when the press got the landing roll ready (and for
    // that press's release), which then doesn't sneak
    bool OnSneakButton(bool aDown, bool aJustPressed) noexcept;
    // A landing roll is ready or just started: the player's landing is the roll's, not a stumble; any thread
    [[nodiscard]] static bool RollsLanding() noexcept;
    // A move is under way for this reference (its collision has to stay off); any thread
    [[nodiscard]] static bool IsMoving(const void* apReference) noexcept;
    // The game's placing of this reference, aDistance away from where it is, is to be skipped: a move places it itself
    // (the idle's animation driven movement would hold it where the move started), or a move just ended and this
    // would take it away in one go (that movement puts it back there once more as it ends); any thread
    [[nodiscard]] static bool HoldsPosition(const void* apReference, float aDistance) noexcept;

    // For the grappling gun (GrappleService), whose winch pulls the local player to a ledge and hands over to the grab,
    // or holds the player hanging on its cable. Main thread.
    // The ledge a grab from the air would take from these feet facing aHeading (radians, the game's: 0 = +y): the
    // point on its top at its edge (Mark), and the feet a high grab of it starts from (Start, the way into its
    // animation: a pull that ends there hands over to the grab)
    struct GrabSpot
    {
        NiPoint3 Mark{};
        NiPoint3 Start{};
    };
    [[nodiscard]] std::optional<GrabSpot> FindGrabSpot(PlayerCharacter* apPlayer, const NiPoint3& acFeet, float aHeading) const noexcept;
    // Grabs the ledge in the hands' reach now, as jump held in the air does: true when the grab started (the move then
    // has the player, and puts its collision back as it ends)
    bool GrabFromAir(PlayerCharacter* apPlayer) noexcept;
    // Whether one of the local player's moves is under way
    [[nodiscard]] bool IsPlayerMoving() const noexcept;
    // Another service moving the local player the way the moves do: its collision off (kept off, no gravity) while it
    // places the player every frame (PlacePlayer), until ReleasePlayer hands it back to the game, carrying on at
    // aVelocity (game units/s; with Actor Velocity Framework)
    static void HoldPlayer(PlayerCharacter* apPlayer) noexcept;
    static void PlacePlayer(PlayerCharacter* apPlayer, const NiPoint3& acPosition) noexcept;
    static void ReleasePlayer(PlayerCharacter* apPlayer, const NiPoint3& acVelocity) noexcept;

private:
    using Move = ParkourMoves::Move;

    // Where a move goes, in the world: what both the player's own and the other players' copies follow
    struct Path
    {
        Move Kind{Move::kCount};
        float Start[3]{};
        // the way the character faces (radians, the game's: 0 = +y)
        float Heading{0.f};
        // where the move's animation expects to start from (eased into from Start), and where it ends: the clip's
        // own travel runs from Ideal; the difference to End is eased in over the move's last part
        float Ideal[3]{};
        float End[3]{};
        // a vault over something higher than the animation clears: extra height in the middle of the move
        float Bump{0.f};
        // where the HUD marker shows the move (not shared): the ledge's edge on its top, or the top of what's vaulted
        float Mark[3]{};
    };

    // A move being played: the local player's (Local), a human NPC's this game runs (Owned), or a copy here of another
    // game's character
    struct Playing
    {
        Path Way{};
        uint32_t ActorId{0};
        // the reference it holds a place in the moving list for (the form may be gone by the time the move ends)
        const void* Reference{nullptr};
        bool Local{false};
        bool Owned{false};
        // the way the character faced as the move started: everyone but the local player turns from it to the move's
        float StartYaw{0.f};
        // which clip's travel it follows (0 third person, 1 first person)
        int View{0};
        float Time{0.f};
        size_t NextCue{0};
        // first person: how far the move has turned the view so far (pitch down, yaw right, radians)
        float EyePitch{0.f};
        float EyeYaw{0.f};
        // the character's weapons are hidden for the move (shown again as it finishes)
        bool WeaponsCulled{false};
        // the player is going on: the rest of the move hurried along, its idle ended with it
        bool Hurried{false};
    };

    struct Ledge;

    // What the look at the human NPCs remembers of one between looks
    struct NpcWatch
    {
        float Position[3]{};
        bool HasPosition{false};
        // how long it has stood where it is (s)
        float Still{0.f};
        // when it's considered for a move again, and when it was last there to look at (m_npcClock)
        float NextTry{0.f};
        float LastSeen{0.f};
        // when it may slide again (m_npcClock)
        float NextSlide{0.f};
    };

    // Where an NPC wants to go that a move could help with: the way there (radians), and who it's after, if anyone
    struct NpcWish
    {
        float Heading{0.f};
        Actor* pChasing{nullptr};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;

    [[nodiscard]] static bool CanMove(PlayerCharacter* apPlayer) noexcept;
    // The move a jump would make here, if any; aExplain logs why not
    [[nodiscard]] std::optional<Path> Plan(PlayerCharacter* apPlayer, bool aAirborne, bool aExplain) noexcept;
    // The vault over what's ahead the way aHeading goes; aStanding: from a stand too (a player has to be running at it)
    [[nodiscard]] std::optional<Path> PlanVault(Actor* apActor, float aHeading, bool aStanding, bool aExplain) const noexcept;
    [[nodiscard]] std::optional<Ledge> FindLedge(PlayerCharacter* apPlayer, bool aAirborne, bool aExplain) const noexcept;
    // The same from feet and a heading of the caller's (the player's own, the grapple's spot under a ledge, an NPC's way)
    [[nodiscard]] std::optional<Ledge> FindLedgeFrom(Actor* apActor, const NiPoint3& acFeet, float aHeading, bool aAirborne,
                                                     bool aExplain) const noexcept;
    // The way of a climb or step onto a ledge from these feet
    [[nodiscard]] static Path PathOntoLedge(Move aKind, const NiPoint3& acFeet, float aHeading, float aScale, const Ledge& acLedge) noexcept;

    // The human NPCs, looked at a few times a second: those a move would help get one (docs/NPC_PARKOUR.md)
    void UpdateNpcs() noexcept;
    [[nodiscard]] std::optional<NpcWish> GetNpcWish(Actor* apActor, const NpcWatch& acWatch, bool aMoving) const noexcept;
    [[nodiscard]] std::optional<Path> PlanNpc(Actor* apActor, const NpcWish& acWish) noexcept;
    bool StartNpc(Actor* apActor, const Path& acPath) noexcept;
    // A slide for an NPC going (unit x, y) at aSpeed: into the cover the game is sending it to, or at its target; true
    // if it slid
    bool TryNpcSlide(Actor* apActor, float aSpeed, float aDirectionX, float aDirectionY) noexcept;

    // The HUD marker: what a jump would do from here, looked for a few times a second, shown on its ledge
    void UpdateMarker(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The landing roll once it's ready: rolls when the player lands, with the momentum it came down with
    void UpdateRoll(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The roll from where the player landed: on along the ground as far as the momentum takes it, short of anything
    // in the way or a drop
    [[nodiscard]] Path PlanRoll(PlayerCharacter* apPlayer) const noexcept;

    void Start(PlayerCharacter* apPlayer, const Path& acPath) noexcept;
    // Advances a move by aDelta and puts its character on the way; false once it's over
    bool Advance(Playing& aMove, float aDelta) noexcept;
    void Finish(Playing& aMove, const char* acpReason) noexcept;

    // The point on a move's way at a time
    static void PointAt(const Path& acPath, int aView, float aTime, float aOut[3]) noexcept;
    // First person: where the view is at a time against where it rests (world x, y, z; game units) and how it's
    // turned (pitch down, yaw right; radians): the clip camera's motion, eased in from rest and out to rest like the
    // way; all 0 in third person
    static void EyeAt(const Path& acPath, int aView, float aTime, float aOut[5]) noexcept;
    TESIdleForm* GetIdle(Move aMove, int aView) noexcept;
    void PlayCues(Playing& aMove, Actor* apActor) noexcept;
    // Tells the others: the local player's move, or the move of an NPC this game runs
    void Share(const Path& acPath, const Actor* apNpc = nullptr) noexcept;

    World& m_world;
    Rules m_rules{};

    // jump control, from the input hook
    std::atomic<bool> m_jumpDown{false};
    // holding jump in the air looks for a ledge to grab, once per hold
    bool m_holdUsed{false};
    float m_retryTimer{0.f};
    // the last step taken, so the next one is with the other leg
    bool m_lastStepLeft{false};
    // the HUD marker's move, and when it's looked for next
    std::optional<Path> m_marker;
    float m_markerTimer{0.f};
    // the landing roll: ready (crouch pressed in the air), how long it stays ready, the press's release still to come,
    // and the player's horizontal velocity coming down (from its positions)
    bool m_rollReady{false};
    float m_rollReadyTime{0.f};
    bool m_swallowSneakRelease{false};
    float m_rollVelocity[2]{};
    float m_rollLastPosition[3]{};
    bool m_rollHasPosition{false};
    // where the game's combat AI is sending NPCs into cover (by form id), as of this look
    std::unordered_map<uint32_t, std::array<float, 3>> m_coverWishes;
    // the human NPCs: when they're looked at next, the time since the last look, and what's known of each by form id
    float m_npcTimer{0.f};
    float m_npcSince{0.f};
    float m_npcClock{0.f};
    std::unordered_map<uint32_t, NpcWatch> m_npcWatch;

    std::vector<Playing> m_playing;
    TESIdleForm* m_idles[static_cast<size_t>(Move::kCount)][2]{};

    std::vector<std::unique_ptr<FileSound>> m_jumpSounds;
    std::vector<std::unique_ptr<FileSound>> m_landSounds;
    std::vector<std::unique_ptr<FileSound>> m_touchSounds;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_movementActionConnection;
};
