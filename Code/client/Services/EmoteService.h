#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

struct World;
struct UpdateEvent;
struct NotifyMovementAction;
struct Actor;
struct TESIdleForm;

/**
 * @brief Emotes (Fallout 4, docs/EMOTES.md): the game's own gestures and poses, played on the local player's character
 * and on the other players' copy of it.
 *
 * Holding the wheel key (G, config/Emotes.ini) brings up the overlay's wheel of eight emotes; letting go over one plays
 * it. Every emote is a chat command too (/salute, /sit; /emotes lists them). A gesture plays once, the player free to
 * walk through it; a pose (sit, kneel, pray, surrender) goes down, holds, and gets up again at a movement key. Another
 * emote picked while one plays takes its place at once. In first person the view stays: a gesture plays its first person
 * clip on the first person arms (the body's clip with the hands brought into the view, Tools/ConvertEmoteAnimations), a
 * pose plays on the body Full Body First Person shows, the view following its head; without that mod the view goes to
 * third person for the emote. A weapon out is put away first and drawn again after; the weapons are hidden meanwhile
 * (the left hand's too, DualWieldService). The animations
 * are dynamic idles, the game's own clips and ours (The Bird, Hands Up!) on the third person graph and our first person
 * clips on the first person one (as the parkour's moves); kEmote movement actions tell the others' games, which play
 * the third person clips on their copy.
 */
struct EmoteService
{
    EmoteService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~EmoteService() noexcept = default;

    TP_NOCOPYMOVE(EmoteService);

    // An emote has the local player's character (no parkour move starts, the left weapon is hidden); main thread
    [[nodiscard]] static bool IsPlayerEmoting() noexcept;

    // The overlay: emotes(request, payload): the wheel's "hover" (payload: the emote's name, "" none), "play" (its name)
    // and "close"; the chat's "command" (payload: what followed the "/": an emote's name, or "emotes" for the list)
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;

private:
    // A chat command: played (or refused, with why in the chat); false when it isn't an emote's
    bool HandleChatCommand(const std::string& acCommand) noexcept;
    enum class Stage : uint8_t
    {
        kStart, // a gesture, or a pose going down
        kHold,  // a pose held
        kStop,  // a pose getting up
    };

    struct Playing
    {
        uint32_t ActorId{};
        uint8_t Emote{};
        Stage Now{Stage::kStart};
        float Time{};
        bool Female{};
        // the dynamic state its clip plays in (kEvents' index; -1 none yet): the next clip goes to the other one
        int8_t State{-1};
        // the local player's, started in first person: a gesture's first person clips on the first person arms, a pose on
        // the body with the view following its head (BodyPose)
        bool FirstPerson{false};
        bool BodyPose{false};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;

    void LoadSettings() noexcept;
    void CheckWheelKey() noexcept;
    void OpenWheel() noexcept;
    void CloseWheel(bool aPlay) noexcept;

    // Starts an emote on the local player's character on the next frame, a weapon out put away first; false with why not
    bool Play(uint8_t aEmote, std::string& aWhyNot) noexcept;
    // The weapon put away for the emote asked for is away (or waited for long enough)
    [[nodiscard]] bool WeaponAway(float aDelta) noexcept;
    void StartLocal() noexcept;
    // The log line of how far the local player's emote moved the hands and head (its check that the clip played)
    void LogMotion(const Playing& acPlaying) noexcept;
    void UpdateLocal(float aDelta) noexcept;
    void UpdateRemote(Playing& aPlaying, float aDelta) noexcept;
    // Plays a stage's clip on an actor (a remote one through the animation sync's block)
    bool PlayClip(Actor* apActor, Playing& aPlaying, Stage aStage, bool aRemote) noexcept;
    void Finish(Playing& aPlaying, bool aRemote) noexcept;
    void Share(uint8_t aEmote, uint8_t aStage) noexcept;
    // the idle for a clip, made once: third person on the body's graph in dynamic state aState (a pose's) or the state
    // that leaves the player walking (a gesture's), or its first person clip (a gesture's, walking too)
    TESIdleForm* IdleFor(const char* acpFile, uint8_t aState, bool aFirstPerson, bool aGesture) noexcept;
    [[nodiscard]] bool MovementPressed() noexcept;

    World& m_world;

    // the local player's emote (ActorId 0: none) and the one asked for, starting on the next frame
    Playing m_local{};
    int m_pendingEmote{-1};
    int m_pendingFrames{0};
    bool m_restoreFirstPerson{false};
    // a weapon out was put away for the emote (drawn again when it ends), and how long the emote asked for still waits
    // for it to be away (-1: not waiting)
    bool m_redraw{false};
    float m_holsterWait{-1.f};
    // third person: the camera turns around the character through a pose (free rotation, as in furniture)
    bool m_freeCamera{false};
    // a first person emote's end: its IdleStop sent again this long after it, and later (-1: none)
    float m_stopAgainIn{-1.f};
    int m_stopsAgain{0};
    // the first person graph's bAnimationDriven turned off this long from now (-1: not), after a load (a new first
    // person model) and after a first person emote
    float m_freeIn{-1.f};
    const char* m_freeWhen{""};
    uint8_t* m_lastFirstPersonRoot{nullptr};
    bool m_movementWasDown{false};
    std::vector<int> m_movementKeys;
    std::vector<Playing> m_remote;

    std::unordered_map<std::string, TESIdleForm*> m_idles;

    int m_wheelKey{'G'};
    std::string m_wheelKeyName{"G"};
    std::array<uint8_t, 8> m_wheel{};
    bool m_wheelKeyDown{false};
    bool m_wheelOpen{false};
    int m_hovered{-1};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_movementActionConnection;
};
