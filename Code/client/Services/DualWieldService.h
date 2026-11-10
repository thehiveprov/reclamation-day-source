#pragma once

#include <Games/Primitives.h>
#include <Events/EventDispatcher.h>
#include <Structs/Inventory.h>
#include <OffHandModel.h>
#include <DualWieldPose.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

struct World;
struct UpdateEvent;
struct OverlayRestartedEvent;
struct NotifyMovementAction;
struct PlayerCharacter;
struct Actor;
struct TESForm;
struct TESObjectREFR;
struct ProjectileLaunchData;
struct BGSKeywordForm;
namespace WeaponGrips
{
struct Grip;
struct ArmClip;
struct LimbClip;
}

/**
 * @brief Dual wielding, as Skyrim has it (docs/DUAL_WIELD.md): the grappling gun, a one-handed melee weapon, a pistol or
 * an SMG in the left hand beside the weapon in the right. The left-hand weapon isn't equipped in the game's sense (the
 * game's firing and its graphs only run the main weapon): it stays in the inventory, put there with a right click in the
 * Pip-Boy's inventory, and this service shows it on WeaponLeft (OffHandModel), poses the arms (DualWieldPose) and fires it
 * through the game's own firing, or swings it with the game's own melee hit. For the game's weapon code the left gun is
 * the player's main hand weapon while it runs (EquipSwap in the .cpp): its cone of fire, perks, sounds, ammo, muzzle
 * flash and VATS are the game's. One mouse button per hand: with a weapon in the left hand, right mouse is the left
 * hand's and never aims down sights; a held key gives two guns a slight zoom instead. A left melee weapon power attacks
 * when right mouse is held, and both buttons held raise a guard (the game's block). In VATS two guns take turns. The
 * grappling gun, in either hand or alone: hold its button to see where the spike lands, let go to fire. With two guns
 * the overlay draws the crosshair (the game's is hidden), which kicks out and closes again after each shot, and the
 * reload lowers both guns out of view. The Pip-Boy's inventory marks the left hand's weapon. The left hand's weapon is
 * kept in the save (RD_DualWield.esp's globals). The left hand's weapon and what it does are sent to the other players,
 * whose copies of the character show them. Fallout 4 only.
 */
struct DualWieldService final : BSTEventSink<TESEquipEvent>
{
    friend struct DualWieldHooks;

    DualWieldService(World& aWorld, entt::dispatcher& aDispatcher);
    ~DualWieldService() noexcept override;

    // A consumable the player uses (the game's equip event): both hands lowered for it (UpdateConsume)
    BSTEventResult OnEvent(const TESEquipEvent* apEvent, const EventDispatcher<TESEquipEvent>*) override;

    TP_NOCOPYMOVE(DualWieldService);

    // What the attack control's hook (PipboyMapService's, AttackBlockHandler) does with an event
    enum class AttackInput : uint8_t
    {
        // on to the game as it is
        kPass,
        // the game never sees it
        kSwallow,
        // the press held back for the grapple's aim goes to the game first (HeldPress), then this event (its release)
        kReplayPress,
        // the game gets a release instead (ReleaseEvent): the right weapon's attack let go as the guard goes up
        kReleaseInstead,
    };
    // From the attack control's hook (main thread): left and right mouse (PrimaryAttack, SecondaryAttack)
    [[nodiscard]] static AttackInput OnAttackButton(const void* apEvent) noexcept;
    // The press kReplayPress hands back to the game (a copy of the game's own, made when it was held back)
    [[nodiscard]] static const void* HeldPress() noexcept;
    // The release kReleaseInstead gives the game (a copy of the event it replaces, let go)
    [[nodiscard]] static const void* ReleaseEvent() noexcept;
    // From the holster and reload control's hook (ReadyWeaponHandler, main thread): watched, never kept from the game
    static void OnReadyWeaponButton(const void* apEvent) noexcept;
    // From the projectile launch hook (any thread), once the game has launched it (aHandle, 0 for none): the local
    // player's own shots kick the crosshair and the right arm; the left gun's are remembered for their muzzle flash
    static void OnLaunch(const ProjectileLaunchData& acData, uint32_t aHandle) noexcept;
    // Whether the launch under way (this thread) is the local player's left gun's (sent so to the other players)
    [[nodiscard]] static bool IsLaunchingLeft() noexcept;
    // Another player's left gun's shot, as CombatService launches it here (main thread): its weapon put in the launch
    // (false: their left hand isn't known, the shot goes as it came), then its projectile remembered for the flash
    static bool PrepareRemoteShot(Actor* apShooter, ProjectileLaunchData& aData) noexcept;
    static void OnRemoteShot(Actor* apShooter, uint32_t aHandle) noexcept;

private:
    enum class Kind : uint8_t
    {
        kNone,
        kGun,
        kMelee,
        kGrapple,
    };

    // The weapon in a hand: its base form, the stack's mods as the game has them (which tell its stack from the others;
    // the network gets them as the server knows them) and its kind
    struct HeldWeapon
    {
        uint32_t FormId{0};
        Vector<OffHandMod> Mods{};
        Kind Type{Kind::kNone};
    };

    // What a left-hand weapon does, read from its stack's instance (its mods applied) when it's put in the hand and
    // again after a workbench may have changed it
    struct Stats
    {
        TESForm* pAmmo{nullptr};
        TESForm* pProjectile{nullptr};
        TESForm* pAttackSound{nullptr};
        TESForm* pDryFireSound{nullptr};
        TESForm* pEquipSound{nullptr};
        void* pInstance{nullptr};
        uint32_t Capacity{0};
        uint32_t Projectiles{1};
        // s between shots (or swings), and whether holding the button keeps firing
        float Interval{0.25f};
        bool Automatic{false};
        float ReloadSeconds{1.5f};
        float FlashSeconds{0.05f};
        std::string MuzzleFlash{};
    };

    // A melee weapon's third person clips (the game's own, their right arm mirrored onto the left): the two standing
    // attacks, the power attack, the block; its idle, its draw and holster (every melee weapon in the
    // left hand with its own animations and grip; moving, the game's own arms)
    struct MeleeLimbClips
    {
        const WeaponGrips::LimbClip* pAttackA{nullptr};
        const WeaponGrips::LimbClip* pAttackB{nullptr};
        const WeaponGrips::LimbClip* pPower{nullptr};
        const WeaponGrips::LimbClip* pBlock{nullptr};
        const WeaponGrips::LimbClip* pIdle{nullptr};
        const WeaponGrips::LimbClip* pDraw{nullptr};
        const WeaponGrips::LimbClip* pHolster{nullptr};
    };
    [[nodiscard]] static MeleeLimbClips ThirdPersonMeleeClips(const BGSKeywordForm* apKeywords) noexcept;

    // Third person moving with a weapon in each hand (the weapons stay in the hands while the game's own jog and sprint
    // play): whether the game's own walk, jog, run or sprint may have the arms at this speed
    // over the ground (units/s). A right gun's only while its arms swing free: its sprint (the same arms as the unarmed
    // one), its relaxed walk and jog (its ready ones hold the gun in both hands, the left hand on it); a right melee
    // weapon's or the fists' always
    [[nodiscard]] static bool GameMovesArms(Actor* apActor, bool aRightGun, float aSpeed, bool aSprinting) noexcept;

    // Another player's left hand as their game sent it, shown on their copy of the character
    struct RemoteHand
    {
        uint32_t ActorId{0};
        HeldWeapon Weapon{};
        std::unique_ptr<OffHandModel> pModel;
        uint8_t* pRoot{nullptr};
        float Weight{0.f};
        float Kick{0.f};
        float Swing{-1.f};
        bool PowerSwing{false};
        bool Guard{false};
        float GuardWeight{0.f};
        float Lower{-1.f};
        // their weapon state last frame, and the left weapon's draw or holster (the reload's dip coming up or going
        // down: s; below 0: none)
        uint32_t WeaponState{0};
        float Draw{-1.f};
        float Holster{-1.f};
        // their left melee weapon's third person clips (looked up for the weapon they hold: LimbsFor), the attack the
        // swing under way plays (the two taken in turn), how much of a clip is on
        uint32_t LimbsFor{0};
        MeleeLimbClips Limbs{};
        bool SwingB{false};
        float LimbWeight{0.f};
        const WeaponGrips::LimbClip* pLastLimb{nullptr};
        float LastLimbTime{0.f};
        // where they were, their speed over the ground (smoothed), how much the game's own movement has their arms
        glm::vec3 LastPosition{0.f};
        float Speed{0.f};
        float GameMove{0.f};
        // their left gun's last shot (s ago), and how much its arm aims at their crosshair
        float SinceShot{10.f};
        float LeftAim{0.f};
        // how much of the pose their right arm takes (their gun beside a left melee weapon: only for the reload)
        float RightWeight{0.f};
        float LastSeen{0.f};
        float AttachTimer{0.f};
        // the copy's muzzle, its gun's weapon data made for (m_remoteData has the data)
        uint8_t* pFireNode{nullptr};
    };

    // A left gun's EquippedWeaponData (the local player's, or another player's for their shots' muzzle flash) and what
    // it was made for
    struct LeftData
    {
        uint32_t ActorId{0};
        uint8_t* pData{nullptr};
        TESForm* pWeapon{nullptr};
        void* pInstance{nullptr};
    };
    // Another player on their grapple's cable with nothing in the left hand: their right arm up it, eased in and out
    struct RemoteHang
    {
        uint32_t ActorId{0};
        uint8_t* pRoot{nullptr};
        float Weight{0.f};
        glm::vec3 Spike{0.f};
    };

    // A left gun's projectile in flight: its handle, who fired it, when
    struct LeftShot
    {
        uint32_t Handle{0};
        uint32_t ActorId{0};
        float Time{0.f};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnOverlayRestarted(const OverlayRestartedEvent&) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;
    void LoadConfig() noexcept;

    // The left hand's weapon: put there (from the Pip-Boy's right click, or back from the save: aQuiet, no message or sound),
    // taken away, checked against the inventory
    void SetLeftWeapon(PlayerCharacter* apPlayer, TESForm* apWeapon, uint32_t aStackIndex, bool aQuiet = false) noexcept;
    void ClearLeftWeapon(PlayerCharacter* apPlayer, const char* acpReason) noexcept;
    void CheckLeftWeapon(PlayerCharacter* apPlayer) noexcept;
    // The left hand's weapon kept in the save: RD_DualWield.esp's three globals written whenever the hand changes, and
    // read back once a save has loaded (false while the inventory doesn't have that stack yet)
    void SaveToGlobals() noexcept;
    bool RestoreFromGlobals(PlayerCharacter* apPlayer) noexcept;
    // The Pip-Boy's inventory: a right click puts the selected weapon in the left hand (or takes it out)
    void UpdatePipboyKey(PlayerCharacter* apPlayer) noexcept;

    // Whether both hands are in use now: a weapon in the left hand, the right one drawn and one-handed or empty (the
    // fists: the left weapon beside them), no Pip-Boy up
    [[nodiscard]] bool IsActive(PlayerCharacter* apPlayer) const noexcept;
    // Whether the left weapon is out (shown in the hand): as IsActive, and also while it's drawn or put away with the
    // right one, and while the Pip-Boy has the left arm (the weapon stays in the hand)
    [[nodiscard]] bool IsOut(PlayerCharacter* apPlayer) const noexcept;
    // The draw and holster: the left weapon comes up and goes down with the right one (first person the game's own
    // clips: the right arm's carried over with two guns, else the left weapon's; third person the reload's dip)
    void UpdateDraw(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The right weapon's plain first person idle (its arm's motion is read from it), looked up when the weapon changes
    void UpdateRightIdle(PlayerCharacter* apPlayer) noexcept;
    // First person, the left gun's own clips' motion (the right hand holding no gun to follow): its draw, its holster,
    // its sprint, run or walk, else its idle with a fidget now and then, one blended into the next
    [[nodiscard]] DualWieldPose::Motion UpdateOwnMotion(float aDelta, bool aSprinting) noexcept;
    // Third person: the left melee weapon's own clip now (its attack or block, the swing's phase mapped onto it so its
    // HitFrame comes with the hit; its draw or holster), eased in and blended clip to clip, and under it its idle, put in
    // the arms
    void SetLimbClip(DualWieldPose::Arms& aArms, float aDelta) noexcept;
    // Third person: how much the game's own walk, jog, run or sprint has the arms now (GameMovesArms; not while either
    // weapon is in use, nor on the cable, nor in first person), the right gun asked into its relaxed state for it while
    // neither is (the game's own Action Gun Relaxed, as the game asks itself after fGunPlayerRelaxedWaitTime); the pose's
    // weight goes down by it
    [[nodiscard]] float UpdateGameMove(PlayerCharacter* apPlayer, float aDelta, bool aSprinting, bool aHanging) noexcept;
    [[nodiscard]] Kind RightKind(PlayerCharacter* apPlayer) const noexcept;
    [[nodiscard]] static bool RightIsEmpty(PlayerCharacter* apPlayer) noexcept;
    // A left weapon put in an empty right hand's company: the fists drawn once the Pip-Boy's closed, as the game draws a
    // weapon equipped from it
    void DrawForLeftHand(PlayerCharacter* apPlayer) noexcept;
    // Test builds: frames where the view does what the mouse didn't ask (a long frame, the view off the player's look,
    // the eye moved on the body), logged with what dual wielding was doing then
    void ProbeView(PlayerCharacter* apPlayer, float aDelta) noexcept;

    void UpdateLeftHand(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // On the motorcycle: only the left hand slot, up while its buttons are held and a while after
    void UpdateBike(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The left melee weapon: right mouse let go early swings, held to the game's power attack delay power attacks, held
    // with left mouse guards
    void UpdateLeftMelee(PlayerCharacter* apPlayer, float aDelta, bool aPressed, bool aReleased) noexcept;
    // The left gun's shot: the game's own (TESObjectWEAP::Fire) with the left gun as the main hand's weapon
    void FireLeft(PlayerCharacter* apPlayer) noexcept;
    // The left swing's hit at its middle: the game's own melee hit, with the left weapon and the race's left (power)
    // attack
    void SwingHit(PlayerCharacter* apPlayer) noexcept;
    void StartSwing(PlayerCharacter* apPlayer, bool aPower) noexcept;
    // The guard (both buttons held with a melee weapon in the left hand): the game's block, its graph's IsBlocking
    void SetGuard(PlayerCharacter* apPlayer, bool aOn) noexcept;
    void FireLeftGrapple(PlayerCharacter* apPlayer) noexcept;
    // The grappling gun in the right hand never reloads: an empty magazine with spikes in the
    // inventory gets one, without the game's reload (its shots keep the magazine from emptying: DualWieldHooks)
    void KeepGrappleLoaded(PlayerCharacter* apPlayer) noexcept;

    // The left gun's EquippedWeaponData, built by the game's own constructor as equipping the gun would (its aim model,
    // fire sounds), kept up to date with the left magazine and the copy's muzzle (false: no left gun to fire); made again
    // when the weapon, its mods or the view's copy change. Its aim model and muzzle flash run every frame.
    bool PrepareLeftData(PlayerCharacter* apPlayer) noexcept;
    void ReleaseLeftData() noexcept;
    void UpdateLeftData(PlayerCharacter* apPlayer) noexcept;
    // Another player's left gun's data (for their shots' muzzle flash), made on their copy's muzzle; let go (aActorId 0:
    // everyone's)
    void PrepareRemoteData(RemoteHand& aHand, Actor* apActor, uint8_t* apRoot) noexcept;
    void ReleaseRemoteData(uint32_t aActorId) noexcept;
    // A reference to that actor's left gun's data with what it was made for, for the game's other threads (no data: none)
    [[nodiscard]] LeftData AcquireLeftData(uint32_t aActorId) noexcept;
    [[nodiscard]] uint8_t* GetLeftFireNode(PlayerCharacter* apPlayer) const noexcept;
    // Who fired that projectile from a left gun (0: no one)
    [[nodiscard]] uint32_t LeftShooterOf(uint32_t aHandle) noexcept;

    // VATS with two guns (alternate hands): the left gun queues every other attack (from the queue's hook) and fires
    // every other attack of the playback (from the weaponFire hook, the game's animation thread)
    [[nodiscard]] bool QueuesLeft(uint32_t aQueued) const noexcept;
    [[nodiscard]] bool FiresLeftInVats(PlayerCharacter* apPlayer) noexcept;
    void UpdateVats(PlayerCharacter* apPlayer) noexcept;

    // The Pip-Boy's inventory list: " [L]" after the left hand's weapon
    void UpdatePipboyMark(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void StartReload(PlayerCharacter* apPlayer, bool aBoth) noexcept;
    // A consumable used with both hands full: the weapons lowered, the pose let go while the game plays the item's own
    // first person animation (a stimpak's, a Stealth Boy's: PlayAnimOnItemUse), then the arms back from below with the
    // weapons. Third person: both weapons culled and the arms the game's while its own third person animation plays
    void UpdateConsume(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // The game's weapon cull a third person consumable took, given back (once)
    void ReleaseConsumeCull(PlayerCharacter* apPlayer) noexcept;
    void UpdateReload(PlayerCharacter* apPlayer, float aDelta) noexcept;
    // How many the left gun's magazine can hold now: its capacity, from what's in the inventory and not in the right
    // gun's magazine
    [[nodiscard]] uint32_t LoadableRounds(PlayerCharacter* apPlayer) const noexcept;
    // The left gun's rounds outside the two magazines (the crosshair's count)
    void UpdateReserve(PlayerCharacter* apPlayer) noexcept;
    // Whether the right gun's magazine has room and the inventory rounds for it
    [[nodiscard]] bool RightNeedsRounds(PlayerCharacter* apPlayer) const noexcept;
    void ReadStats(PlayerCharacter* apPlayer) noexcept;

    // The arms: both hands' holds while dual wielding, the grappling gun's arm up the cable while hanging on it
    void UpdatePose(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void UpdateZoom(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void UpdateCrosshair(PlayerCharacter* apPlayer) noexcept;
    void UpdateRemotes(float aDelta) noexcept;

    void SendState() noexcept;
    void SendCue(uint8_t aCue) noexcept;

    World& m_world;

    HeldWeapon m_left{};
    // its grip, the game's own first person animations' for it (WeaponGrips; null: the left hand keeps the game's), and a
    // melee weapon's third person one (its third person idle's: the hand its third person attacks hold it in)
    const WeaponGrips::Grip* m_pLeftGrip{nullptr};
    const WeaponGrips::Grip* m_pLeftGrip3P{nullptr};
    // a left melee weapon's first person clips (its folders', as the game would play them in the right hand): its idle,
    // the two attacks taken in turn, the power attack, the block
    struct MeleeClips
    {
        const WeaponGrips::ArmClip* pIdle{nullptr};
        const WeaponGrips::ArmClip* pAttackA{nullptr};
        const WeaponGrips::ArmClip* pAttackB{nullptr};
        const WeaponGrips::ArmClip* pPower{nullptr};
        const WeaponGrips::ArmClip* pBlock{nullptr};
        // its draw and holster, its run (walking too) and sprint
        const WeaponGrips::ArmClip* pDraw{nullptr};
        const WeaponGrips::ArmClip* pHolster{nullptr};
        const WeaponGrips::ArmClip* pRun{nullptr};
        const WeaponGrips::ArmClip* pSprint{nullptr};
    };
    MeleeClips m_meleeClips{};
    // the player's speed over the ground (units/s, smoothed: the left weapon's walk and run clips, the game's own movement)
    // and where they were
    float m_speed{0.f};
    glm::vec3 m_lastPosition{0.f};
    // third person: how much the game's own movement has the arms, s to the next ask for the relaxed gun; s since the left
    // mouse's last press went to the game (the right weapon in use); the game's melee attack state, and its gun state with
    // the graph's (<< 8), last frame (logged as they change)
    float m_gameMove{0.f};
    float m_relaxRetry{0.f};
    float m_sinceRightPress{10.f};
    uint32_t m_meleeAttackState{0};
    uint32_t m_gunState{0};
    MeleeLimbClips m_meleeLimbs{};
    // the third person clip shown last (and the one before it, blended out), how much of it is on
    const WeaponGrips::LimbClip* m_pLimbShown{nullptr};
    float m_limbShownTime{0.f};
    const WeaponGrips::LimbClip* m_pLimbFrom{nullptr};
    float m_limbFromTime{0.f};
    float m_limbBlend{1.f};
    float m_limbWeight{0.f};
    // a left gun's first person clips (its folders'), its arm's motion when the right hand has no gun to follow: its
    // idle, the idle's fidgets (A to D), its draw and holster
    struct GunClips
    {
        const WeaponGrips::ArmClip* pIdle{nullptr};
        std::array<const WeaponGrips::ArmClip*, 4> Fidgets{};
        uint32_t FidgetCount{0};
        const WeaponGrips::ArmClip* pDraw{nullptr};
        const WeaponGrips::ArmClip* pHolster{nullptr};
        const WeaponGrips::ArmClip* pWalk{nullptr};
        const WeaponGrips::ArmClip* pRun{nullptr};
        const WeaponGrips::ArmClip* pSprint{nullptr};
    };
    GunClips m_gunClips{};
    // its own clip shown last (its motion blended from the one before: m_ownFrom, m_ownBlend), the idle's or a fidget's
    // time and the idle's loops until the next fidget
    const WeaponGrips::ArmClip* m_pOwnClip{nullptr};
    DualWieldPose::Motion m_ownFrom{};
    DualWieldPose::Motion m_ownLast{};
    float m_ownBlend{1.f};
    const WeaponGrips::ArmClip* m_pIdleClip{nullptr};
    float m_idleTime{0.f};
    uint32_t m_idleLoops{0};
    uint32_t m_loopsToFidget{3};
    // the right weapon's plain first person idle (its arm's motion is read from it) and the keywords it was looked up for;
    // how much of that motion the right and left guns' holds take (two guns, first person), the right gun's last shot,
    // and whether its emptied magazine has had its lowered reload
    const WeaponGrips::ArmClip* m_pRightIdle{nullptr};
    const void* m_pRightIdleFor{nullptr};
    float m_rightFollow{0.f};
    float m_leftFollow{0.f};
    float m_sinceRightShot{10.f};
    bool m_rightDryDipped{false};
    // the draw and holster: the game's weapon state last frame (CommonLibF4 WEAPON_STATE), the left weapon's draw or
    // holster under way (s; below 0: none) and how long its motion lasts
    uint32_t m_weaponState{0};
    float m_drawTime{-1.f};
    float m_drawLength{0.f};
    float m_holsterTime{-1.f};
    float m_holsterLength{0.f};
    // the swing under way: its clip, how long it runs at the weapon's speed, where in it the hit comes and where the
    // next swing may start (phases); the next swing's clip is the other attack
    const WeaponGrips::ArmClip* m_pSwingClip{nullptr};
    float m_swingLength{0.45f};
    float m_swingHitAt{0.5f};
    float m_swingChainAt{1.f};
    bool m_swingAlternate{false};
    // the block clip's time (raised to its hold, back down when let go), the clip shown last and the one it blends from
    float m_guardTime{0.f};
    const WeaponGrips::ArmClip* m_pShownClip{nullptr};
    float m_shownClipTime{0.f};
    const WeaponGrips::ArmClip* m_pClipFrom{nullptr};
    float m_clipFromTime{0.f};
    float m_clipBlend{1.f};
    // how much of the pose the right arm takes (two guns; a gun beside a melee weapon only for the reload's dip)
    float m_rightWeight{0.f};
    // a consumable used (from the equip event, taken in the update), the lowering for it under way (s; below 0: none),
    // how long the game's animation for it holds the arms, how much of the pose is on meanwhile
    std::atomic<uint32_t> m_consumeItem{0};
    float m_consume{-1.f};
    float m_consumeHold{0.f};
    bool m_consumeRelease{false};
    float m_consumeWeight{1.f};
    // third person (as it began): no lowering, both weapons culled while the game's own third person animation for it
    // plays, the arms the game's; that animation's length (s); the cull ours to take back
    bool m_consumeThirdPerson{false};
    float m_consumeThirdPersonLength{0.f};
    bool m_consumeCulled{false};
    Stats m_stats{};
    std::unique_ptr<OffHandModel> m_pModel;
    // the roots the left weapon's copies and the arms' pose are on (the first person and third person skeletons)
    uint8_t* m_pFirstRoot{nullptr};
    uint8_t* m_pThirdRoot{nullptr};
    uint8_t* m_pPosedRoot{nullptr};

    // this frame's: both hands in use, the left weapon out, and what's in the right one (the input hooks ask afresh)
    bool m_activeNow{false};
    bool m_shownNow{false};
    Kind m_rightNow{Kind::kNone};
    bool m_rightEmpty{false};
    // the fists to draw for a left weapon put in the hand with the right one empty
    bool m_drawPending{false};

    // the view probe's running picture: the usual frame time, how far off the look the view usually is and how far it
    // usually turns in a frame, last frame's look and eye (on the body), and how many lines it's logged this minute
    struct ViewProbeState
    {
        bool Primed{false};
        float UsualDelta{0.f};
        float UsualOff{0.f};
        float UsualTurn{0.f};
        float LastYaw{0.f};
        float LastPitch{0.f};
        glm::vec3 LastEye{0.f};
        float MinuteTime{0.f};
        uint32_t Frames{0};
        float Slowest{0.f};
        uint32_t Logged{0};
    };
    ViewProbeState m_probe{};

    uint32_t m_magazine{0};
    bool m_magazineKnown{false};
    int64_t m_reserve{0};
    // the left button: down now, pressed since the last update, let go since the last update
    bool m_leftDown{false};
    bool m_leftPressed{false};
    bool m_leftReleased{false};
    // riding the motorcycle (BikeLink): the left hand slot's seconds left up, the sights held and how far up to them, and
    // a spike fired from it still out
    bool m_onBike{false};
    float m_bikeUp{0.f};
    bool m_bikeAds{false};
    float m_bikeAdsWeight{0.f};
    bool m_bikeLine{false};
    // the right button held back for a grapple in the right hand (its press copied for the replay)
    bool m_rightGrappleHeld{false};
    alignas(16) uint8_t m_heldPress[0x40]{};
    float m_sinceShot{10.f};
    float m_swing{-1.f};
    bool m_swingHit{false};
    bool m_powerSwing{false};
    // the left melee weapon's hold: how long right mouse has been down (a power attack at the game's
    // fInitialPowerAttackDelay), and whether this hold has swung or guarded already
    float m_leftHeld{0.f};
    bool m_holdUsed{false};
    // the guard: up now, how far the arm is in it; left mouse down, and whether the game had its press
    bool m_guard{false};
    float m_guardWeight{0.f};
    bool m_primaryDown{false};
    bool m_primaryGiven{false};
    bool m_primarySwallowed{false};
    alignas(16) uint8_t m_releaseEvent[0x40]{};

    // the left gun's EquippedWeaponData (ours: one reference) and what it was made for, the other players' left guns'
    // and the left guns' projectiles in flight; the lock keeps them for the game's other threads
    uint8_t* m_pLeftData{nullptr};
    TESForm* m_pLeftDataWeapon{nullptr};
    void* m_pLeftDataInstance{nullptr};
    uint8_t* m_pLeftFireNode{nullptr};
    std::mutex m_leftDataLock;
    Vector<LeftData> m_remoteData;
    Vector<LeftShot> m_leftShots;

    // VATS: two guns ready for the left gun's turns (set every frame), the longest queue seen in this playback, and what
    // the left gun's shots in it did (from the game's thread, taken in the update)
    std::atomic<bool> m_vatsLeftReady{false};
    std::atomic<uint32_t> m_vatsLongest{0};
    std::atomic<uint32_t> m_vatsLeftShots{0};
    std::atomic<uint32_t> m_vatsMagazine{0};

    // the Pip-Boy's mark: checked this often while the inventory is up
    float m_markTimer{0.f};
    // the lowered reload: its phase (below 0: none), how long it takes, whether the right gun goes down with it
    float m_reload{-1.f};
    float m_reloadSeconds{1.5f};
    bool m_reloadBoth{false};
    bool m_reloadTapped{false};
    float m_leftKick{0.f};
    float m_rightKick{0.f};
    float m_weight{0.f};
    // how much of the left hand's grip is on (it stays while the Pip-Boy has the arm)
    float m_gripWeight{0.f};
    // third person, how much the left gun's arm aims at the crosshair however the right one is (after its shots, its
    // button held, hanging from the grapple)
    float m_leftAim{0.f};
    float m_attachTimer{0.f};
    float m_checkTimer{0.f};
    float m_sendTimer{0.f};
    float m_time{0.f};
    // the player in the world (a load in between: the save's left hand is read back), and how long the read back has
    // been waiting for the inventory
    bool m_hadPlayer{false};
    bool m_restorePending{false};
    float m_restoreTime{0.f};

    // the zoom: its key, how far in it is now, the field of view it started from and the one it wrote last (another
    // value there is a change made meanwhile, in the settings: the zoom goes on from that)
    int m_zoomKey{'Z'};
    float m_zoom{0.f};
    float m_baseFov{0.f};
    float m_writtenFov{0.f};
    // a right click in the Pip-Boy's inventory (from the controls' input hook, taken in the update)
    std::atomic<bool> m_pipboyClick{false};

    // the overlay's crosshair: shown, the shots it has kicked for, what it was sent last
    bool m_crosshair{false};
    uint32_t m_shots{0};
    std::string m_sentCrosshair;

    Vector<RemoteHand> m_remotes;
    Vector<RemoteHang> m_remoteHangs;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_overlayRestartedConnection;
    entt::scoped_connection m_movementActionConnection;
};
