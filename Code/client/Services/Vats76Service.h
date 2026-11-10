#pragma once

#if TP_FALLOUT4

#include <Events/UpdateEvent.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct World;
struct Actor;
struct PlayerCharacter;
struct TESObjectREFR;
struct ProjectileLaunchData;

/**
 * RD VATS 76': VATS without the pause, as in Fallout 76, based on Zzyxzz's VATS Unchained (Nexus 109665) and rebuilt in
 * our own code. The VATS button locks on instead of opening the VATS menu; the game keeps running at full speed, so every
 * game of a session stays in step. The enemy and body part nearest the crosshair are picked, the hit chance shows on them
 * in the HUD, and shots are the game's own VATS shots: rolled once per attack with the game's own hit chance, then launched
 * at the part or past it. An automatic weapon fires one VATS burst per trigger pull. Attacks cost the game's own VATS Action
 * Points. As in the game's VATS, the AP and critical meters show while it's on, attacks that hit fill the critical meter,
 * the critical key (MCM; Fallout 76's jump key when none is set) readies a critical strike once it's full, and the picked
 * part glows with the game's own VATS target glow inside a bracket that fits it on the screen. With the perk, the game's
 * Mysterious Stranger can come to finish off a target an attack leaves low on health: a character of his own each visit,
 * the player's summon in co-op.
 *
 * Everything stands down while VATS Unchained itself is installed, so a player who still has it keeps exactly what they had.
 * docs/VATS76.md has the game's side of it.
 */
class Vats76Service
{
public:
    // MCM's ([Vats76] in Data/MCM/Settings/ReclamationDay.ini over these)
    struct Settings
    {
        bool Enabled{true};
        // the highest hit chance, 0..1 (critical strikes always hit)
        float MaxHitChance{0.95f};
        bool Bursts{true};
        // the game's VATS perks work here: Concentrated Fire, Gun Fu, Grim Reaper's Sprint, Demolition Expert
        bool Perks{true};
        // a melee attack jumps to a target out of reach, as in the game's VATS
        bool MeleeJump{true};
        bool Sounds{true};
        // never pick these while they're not hostile to the player
        bool ExcludeCompanions{true};
        bool ExcludeDogmeat{true};
        bool ExcludeSettlers{true};
        // mines and thrown grenades can be picked, as in the game's VATS
        bool TargetExplosives{true};
        // seconds it stays on after the last target is gone (a new one in sight is picked meanwhile)
        float EndDelay{1.5f};
        bool ShowAllParts{true};
        // the picked part glows as in the game's VATS
        bool HighlightPart{true};
        // "Critical strike ready" when the meter fills while it's on
        bool CriticalReadyMessage{true};
        // the game's Mysterious Stranger comes, as in its VATS (with the perk)
        bool Stranger{true};
    };

    Vats76Service(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~Vats76Service() noexcept;

    TP_NOCOPYMOVE(Vats76Service);

    // Whether it's locked on right now (any thread)
    [[nodiscard]] static bool IsActive() noexcept;
    // From the launch hook, on whichever thread the game fires on: a shot of the local player's while it's on becomes a
    // VATS shot at the picked part
    static void AimLaunch(ProjectileLaunchData& aData) noexcept;
    // From the launch hook, once the game has made the projectile AimLaunch aimed: a critical strike's carries the game's
    // own critical flag
    static void OnLaunched(uint32_t aProjectileHandle) noexcept;
    // The game's main loop (the frame's update isn't always on the main thread): a melee jump moves the player here
    static void OnMainLoop() noexcept;
    // From the jump control (ParkourService): true takes the press, as the critical key when it's the jump key (Fallout
    // 76's Space), only while it's on and a critical strike is there to use
    static bool TakeJump(bool aDown, bool aJustPressed) noexcept;

    // The Mysterious Stranger's character on this machine (his NPC, a temporary reference): the player's own, as a summon
    // is in co-op, so the server takes his copies away for everyone as he goes (CharacterService)
    [[nodiscard]] static bool IsStranger(const Actor* apActor) noexcept;

    // The hooks' way in (see Vats76Service.cpp)
    static bool TakesVatsButton() noexcept;
    static void OnVatsButton() noexcept;
    static void OnAttackStarted() noexcept;
    static bool SwallowsAttackRelease() noexcept;
    static bool SwallowsAttackStart() noexcept;

private:
    // A body part that can be picked: one per name, as the game's VATS has them (an arm is two segments of the body part
    // data: its nodes and types are all of them)
    struct Part
    {
        const void* pPart{nullptr};
        std::string Name;
        uint8_t* pNode{nullptr};
        uint32_t Limb{0};
        float Chance{0.f};
        std::vector<uint8_t*> Nodes;
        std::vector<uint8_t> Types;
    };

    // Someone or something that could be picked, looked at a few times a second
    struct Candidate
    {
        uint32_t Handle{0};
        float ScreenDistance{0.f};
        bool Hostile{false};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void UpdateSettings(float aDelta) noexcept;
    void Toggle(PlayerCharacter* apPlayer) noexcept;
    void TurnOn(PlayerCharacter* apPlayer) noexcept;
    // aStrangerStays: a Mysterious Stranger on his way, or here, finishes his visit (out of Action Points, the target's gone)
    void TurnOff(const char* apWhy, bool aSound, bool aStrangerStays = false) noexcept;
    void RefreshCandidates(PlayerCharacter* apPlayer) noexcept;
    void PickTarget(PlayerCharacter* apPlayer) noexcept;
    void BuildParts(Actor* apTarget) noexcept;
    void BuildExplosivePart(TESObjectREFR* apTarget) noexcept;
    void RefreshChances(PlayerCharacter* apPlayer, TESObjectREFR* apTarget) noexcept;
    void PickPart() noexcept;
    void ShowOnHud(TESObjectREFR* apTarget) noexcept;
    void PublishShot(TESObjectREFR* apTarget) noexcept;
    void UpdateBurst(PlayerCharacter* apPlayer) noexcept;
    void PayActionPoints(PlayerCharacter* apPlayer) noexcept;
    void ReadCriticalKey() noexcept;
    void UpdateCritical(PlayerCharacter* apPlayer) noexcept;
    void ToggleCritical() noexcept;
    void UpdateHighlight(TESObjectREFR* apTarget) noexcept;
    // Where a part is on the screen (0..1): the box round its bones' segments, as thick as the part
    [[nodiscard]] bool PartBox(const Part& acPart, float& aX, float& aY, float& aWidth, float& aHeight) const noexcept;
    // The Mysterious Stranger: the rolls the attacks asked for, his visit, his copies gone after it (Vats76Service.cpp)
    void UpdateStranger(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void RollStranger(PlayerCharacter* apPlayer, uint32_t aTarget) noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;

    Settings m_settings{};
    float m_settingsTimer{0.f};
    int m_standingDown{-1};

    // the frame's update (RD's updates run inside the game's script VM update: docs/VATS76.md)
    std::vector<Candidate> m_candidates;
    float m_refreshTimer{0.f};
    uint32_t m_target{0};
    uint32_t m_partsTarget{0};
    // the target's 3D the parts' nodes are in: a new one (3D reloaded) means they're built again
    const void* m_partsRoot{nullptr};
    std::vector<Part> m_parts;
    int m_part{-1};
    std::chrono::steady_clock::time_point m_lastPartSound{};
    float m_noTargetTime{0.f};
    // it started with no one in sight: it waits for the first one
    bool m_waitingForFirst{false};

    // the critical key from MCM's keybinds (Windows virtual key, MCM's modifier bits), 0: the jump key
    int m_criticalKey{0};
    uint32_t m_criticalModifiers{0};
    bool m_criticalKeyDown{false};
    std::filesystem::file_time_type m_keybindsTime{};
    // the meter was full the last frame (the "ready" note is for the moment it fills)
    bool m_criticalWasFull{false};
};

#endif
