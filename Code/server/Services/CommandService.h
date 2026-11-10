#pragma once

#include <Events/PacketEvent.h>

struct World;
struct TeleportCommandRequest;
struct SetTimeCommandRequest;
struct RequestDifficulty;
struct RequestMovementRules;

/**
 * @brief Processes incoming commands.
 */
struct CommandService
{
    CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CommandService() noexcept = default;

    TP_NOCOPYMOVE(CommandService);

protected:
    void OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept;
    // The host's difficulty becomes the server's, as its weather and time do
    void OnRequestDifficulty(const PacketEvent<RequestDifficulty>& acMessage) const noexcept;
    // So do its movement and parkour rules
    void OnRequestMovementRules(const PacketEvent<RequestMovementRules>& acMessage) const noexcept;
    /**
     * @brief Returns the location of the target player of the teleport command.
     */
    void OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept;

private:
    World& m_world;

    entt::scoped_connection m_setTimeConnection;
    entt::scoped_connection m_teleportConnection;
    entt::scoped_connection m_difficultyConnection;
    entt::scoped_connection m_movementRulesConnection;
};
