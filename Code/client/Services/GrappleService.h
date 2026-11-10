#pragma once

#include <Games/Primitives.h>
#include <Interface/HudMarker.h>

#include <mutex>

struct World;
struct UpdateEvent;
struct NotifyMovementAction;
struct PlayerCharacter;
struct Actor;
struct TESForm;
struct TESObjectREFR;
struct ProjectileLaunchData;

/**
 * @brief The grappling gun (RD_GrapplingGun.esp, Tools/BuildGrapplePlugin.js; docs/GRAPPLE.md). Firing it, the spike on
 * the gun leaves it on a steel cable: the game's projectile isn't launched for it, this service flies the spike (the
 * gun's own, as an object of its own) along the shot and draws the cable from the gun to it. A wall: the winch pulls the
 * player there; at a ledge (the parkour's ledge rules) the pull ends in the parkour's grab, which climbs onto it;
 * anywhere else the player hangs on the cable, the gun's arm up the cable (forward and back climb it, the sides swing,
 * jump lets go, or pulls up onto a ledge the hands reach). A person or creature: the spike's hit is the game's own (the
 * gun's projectile, launched at them as it bites), and the winch reels them to the player; whoever's game runs them
 * (another player's, or another player themselves) reels them. Then the cable reels the spike back onto the gun. Holding
 * the gun's button (DualWieldService holds the shot back until it's let go) the HUD marks where the spike would land,
 * with the climb marker on a ledge the shot would pull up onto. The gun can be in either hand (docs/DUAL_WIELD.md), and
 * a High Velocity Spike Chamber fires the High Velocity spikes (60 m). The other players see the spike and the cable
 * (kGrapple movement actions). Fallout 4 only.
 */
struct GrappleService
{
    GrappleService(World& aWorld, entt::dispatcher& aDispatcher);
    ~GrappleService() noexcept;

    TP_NOCOPYMOVE(GrappleService);

    // From the projectile launch hook (any thread): true when it is the local player's grapple spike, which this
    // service then shoots itself (the hook launches nothing, and sends nothing). The spike's hit on someone it bites
    // (launched by this service) goes through.
    static bool TakeLaunch(const ProjectileLaunchData& acData) noexcept;
    // From the jump control (ParkourService's hook, main thread): true when the event was the grapple's (the player is
    // on its cable: a press lets go, or grabs a ledge in reach)
    static bool TakeJump(bool aJustPressed) noexcept;
    // The HUD marker is the grapple's while its gun is drawn or its cable is out (the parkour's otherwise); main thread
    [[nodiscard]] static bool OwnsMarker() noexcept;
    // The winch holds the local player (pulled, hanging, or reeled in by another player's): none of the parkour's moves
    // start meanwhile; main thread
    [[nodiscard]] static bool HoldsPlayer() noexcept;
    // From DualWieldService every frame (main thread): the gun's button is held (the HUD marks where the spike would
    // land, in the reach of apAmmo, the spikes that gun fires), and the gun is in the left hand (drawn there, though the
    // game's weapon is another)
    static void SetAiming(bool aHeld, bool aOffHand, const TESForm* apAmmo) noexcept;
    // The spike is off the gun (flying, biting, coming back); main thread
    [[nodiscard]] static bool IsLineOut() noexcept;
    // Where the cable is fixed while the winch pulls the player or the player hangs on it: the gun's arm reaches up along
    // it (DualWieldPose), the left arm with the gun in the left hand; false otherwise
    [[nodiscard]] static bool GetHang(glm::vec3& aAnchor, bool& aOffHand) noexcept;
    // The other players whose spike is fixed somewhere (pulled along or hanging on their cable): their character here
    // and where the spike is
    [[nodiscard]] static Vector<std::pair<uint32_t, glm::vec3>> GetRemoteHangs() noexcept;

private:
    enum class Stage : uint8_t
    {
        kIdle,
        // the spike on its way along the shot
        kFlying,
        // the winch pulling the player to where the spike bit
        kPulling,
        // the player on the cable
        kHanging,
        // a person or creature hooked, the winch reeling it in (here, or by the game that runs them)
        kReeling,
        // the spike coming back onto the gun
        kRetracting,
    };

    // A shot as the launch hook saw it, picked up on the next update: where from and which way, the spike's speed and
    // reach, and the gun, spikes and projectile it was (for the hit on someone it bites)
    struct Shot
    {
        NiPoint3 Origin{};
        NiPoint3 Direction{};
        float Speed{0.f};
        float Range{0.f};
        TESForm* pWeapon{nullptr};
        void* pInstance{nullptr};
        TESForm* pAmmo{nullptr};
        TESForm* pProjectile{nullptr};
    };

    struct Line
    {
        Stage Now{Stage::kIdle};
        uint32_t CellId{0};
        NiPoint3 Origin{};
        NiPoint3 Direction{};
        float Speed{0.f};
        float Range{0.f};
        // the gun, spikes and projectile of the shot
        TESForm* pWeapon{nullptr};
        void* pInstance{nullptr};
        TESForm* pAmmo{nullptr};
        TESForm* pProjectile{nullptr};
        // how far the spike has flown, and how far it goes (where it bites, or its reach)
        float Flown{0.f};
        float Length{0.f};
        bool Bites{false};
        // where it bit, and whether that was on top of something (a floor, a roof) rather than a wall
        NiPoint3 Hit{};
        bool HitTop{false};
        // the spike's origin (the shaft's rear) and the way its tip points
        NiPoint3 Spike{};
        NiPoint3 SpikeAhead{};
        // a hooked person or creature, the spike's place on it; reeled by the game that runs them (another player's)
        uint32_t HookedId{0};
        NiPoint3 HookOffset{};
        float HookTime{0.f};
        bool ReeledElsewhere{false};
        // the pull: where it ends, how fast it goes now, and whether a ledge waits there (the grab, facing Heading)
        NiPoint3 PullEnd{};
        float PullSpeed{0.f};
        bool ToLedge{false};
        float Heading{0.f};
        // the hang: where the cable is fixed and how long it is, the hands and how they move
        NiPoint3 Anchor{};
        float Cable{0.f};
        NiPoint3 Hands{};
        NiPoint3 Velocity{};
        // where the player was put last (a load or a teleport moves it far from there)
        NiPoint3 Placed{};
        // the spike and the cable in the world (form ids of the references placed for them)
        uint32_t SpikeId{0};
        uint32_t CableId{0};
    };

    // Another player's line, as their game sent it, drawn on their copy of the character
    struct RemoteLine
    {
        // their character here, and who their spike hooked (here, 0: no one)
        uint32_t ShooterId{0};
        uint32_t HookedId{0};
        NiPoint3 From{};
        NiPoint3 To{};
        float Speed{0.f};
        float Flown{0.f};
        bool Retracting{false};
        NiPoint3 Spike{};
        NiPoint3 SpikeAhead{};
        float Age{0.f};
        uint32_t SpikeId{0};
        uint32_t CableId{0};
    };

    // Someone this game runs, hooked by another player's spike: reeled toward them (the local player is held, an NPC
    // dragged)
    struct ReeledIn
    {
        uint32_t ActorId{0};
        uint32_t ShooterId{0};
        float Time{0.f};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;

    void Fire(PlayerCharacter* apPlayer, const Shot& acShot) noexcept;
    void Advance(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The spike got where it goes: it bites (a wall, someone) or comes back
    void Arrive(PlayerCharacter* apPlayer) noexcept;
    // The spike's hit on who it bit: the gun's own projectile, launched at them from just short of them
    void HitHooked(PlayerCharacter* apPlayer, Actor* apActor) noexcept;
    void Pull(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The pull got where it ends: the grab onto the ledge there, or the hang
    void EndPull(PlayerCharacter* apPlayer) noexcept;
    void StartHanging(PlayerCharacter* apPlayer) noexcept;
    void Hang(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // Jump on the cable: a ledge the hands reach is grabbed, else the player lets go
    void JumpOff(PlayerCharacter* apPlayer) noexcept;
    void LetGo(PlayerCharacter* apPlayer, const NiPoint3& acVelocity, const char* acpReason) noexcept;
    void Reel(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void StartRetracting(const char* acpReason) noexcept;
    void Retract(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void Finish(PlayerCharacter* apPlayer, const char* acpReason) noexcept;
    // Puts the held player's feet here
    void Place(PlayerCharacter* apPlayer, const NiPoint3& acFeet) noexcept;
    // The HUD marker where the spike would land (on a ledge the gun would pull up onto, the climb marker), looked for a
    // few times a second while the gun's button is held
    void UpdateMarker(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // Where a shot's spike biting at acHit leaves a grab onto a ledge facing aHeading: the feet the grab starts from,
    // and where the ledge's edge is (the marker)
    [[nodiscard]] bool FindLedge(PlayerCharacter* apPlayer, const NiPoint3& acHit, float aHeading, NiPoint3& aStart, NiPoint3& aMark) const noexcept;
    // The spike and the cable drawn where the line has them
    void Draw(PlayerCharacter* apPlayer) noexcept;
    [[nodiscard]] bool IsGunDrawn(PlayerCharacter* apPlayer) const noexcept;

    // The line sent to the other players (RequestMovementAction kGrapple)
    void SendLine(uint8_t aStage, const NiPoint3& acFrom, const NiPoint3& acTo, float aSpeed, uint32_t aTarget) noexcept;
    // The other players' lines: flown, held, reeled back and drawn on their copies
    void UpdateRemoteLines(float aDelta) noexcept;
    // Who this game runs that another player's spike hooked: reeled toward that player
    void UpdateReeledIn(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void EndReeledIn(PlayerCharacter* apPlayer, size_t aIndex, const char* acpReason) noexcept;
    // A save made with a spike out kept its visuals: any in the player's cell that no line here has are taken away
    void ClearStrayVisuals(PlayerCharacter* apPlayer) noexcept;

    World& m_world;
    Line m_line{};
    // the winch has the player (ParkourService holds it for us); off while the parkour's grab takes over
    bool m_holding{false};
    bool m_ownsMarker{false};
    float m_markerTimer{0.f};
    std::optional<NiPoint3> m_marker;
    HudMarker::Icon m_markerIcon{HudMarker::Icon::kTarget};
    bool m_markerShown{false};
    // the gun's button held (the aim's marker), the gun in the left hand (DualWieldService), the spikes it fires
    bool m_aiming{false};
    bool m_offHand{false};
    const TESForm* m_pAimAmmo{nullptr};
    // the jump press the line takes (from the input hook, read by the next update)
    bool m_jumpPressed{false};
    // the shot the launch hook took (any thread), for the next update
    std::mutex m_shotLock;
    std::optional<Shot> m_pendingShot;
    // the other players' lines, who this game runs being reeled in by them, and the cell checked for stray visuals
    Vector<RemoteLine> m_remoteLines;
    Vector<ReeledIn> m_reeledIn;
    uint32_t m_checkedCellId{0};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_movementActionConnection;
};
