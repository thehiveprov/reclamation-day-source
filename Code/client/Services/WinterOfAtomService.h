#pragma once

struct World;
struct UpdateEvent;

/**
 * @brief Winter of Atom's shared state the prequel's scripts read, written into its globals once a second.
 *
 * RDWoA_PipboysOnTable: how many Pip-Boys the prologue trader's table shows, one per player still without one. This
 * game's player counts when they wear none; every other player on the server counts until their character, as this
 * game sees it, has been seen wearing one (worn items are synced, a Pip-Boy among them), so each one taken or handed
 * over leaves the table in every game. Out of co-op, only this game's player.
 *
 * The prologue at night: the host's game sets the server's time to 22:00 (as /settime) once per session while a new
 * character's prologue runs (the start done, the prologue not), since the server's time replaces the game's when co-op
 * connects. Fallout 4 only.
 */
struct WinterOfAtomService
{
    WinterOfAtomService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~WinterOfAtomService() noexcept = default;

    TP_NOCOPYMOVE(WinterOfAtomService);

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void UpdatePipboysOnTable() noexcept;
    void UpdatePrologueNight() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    double m_sinceUpdate{0.0};
    // the other players seen wearing a Pip-Boy, by player ID
    TiltedPhoques::Set<uint32_t> m_hadPipboy;
    // the prologue's night was asked for in this session
    bool m_nightSet{false};
};
