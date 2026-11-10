#pragma once

#include "Message.h"

#include <Structs/NemesisSpawn.h>

#include <algorithm>

// Nemeses (Fallout 4, see docs/NEMESIS.md): what the server tells games. See RequestNemesis.
struct NotifyNemesis final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyNemesis;

    enum Action : uint8_t
    {
        kSpawn,        // Spawn: an encounter for this player
        kIdentity,     // to everyone: the character ActorServerId is nemesis RecordId: Text = his name, Kind = rank,
                       // Value = his Identity (PackIdentity)
        kNotice,       // Text: a line for this player's HUD (Kind: Notice)
        kUi,           // Text2: the overlay's event, Text: its JSON
        kOrder,        // to every game: the character ActorServerId (the game that runs it acts), Kind = Order,
                       // Value = the player's server id it concerns (kAttack)
        kKarma,        // Value: a karma change for this player through KARMA (Kind 1: silent)
        kEndEncounter, // RecordId = encounter id: it's over for the server (a man was killed elsewhere, a reset)
    };

    enum Notice : uint8_t
    {
        kNormal,
        kRises,    // a nemesis is born or rises in rank
        kDeath,    // a nemesis died
        kWorld,    // world news (a war, a new leader)
    };

    enum Order : uint8_t
    {
        kSurrender, // hands up, stops fighting
        kFlee,      // runs from the players
        kCalm,      // stops fighting, walks off (backed down)
        kAttack,    // fights the player Value
        kLetGo,     // a man who gave up, spared: hands down, runs
        kLetGoFight // a man who gave up, spared: hands down, turns on the player Value anyway
    };

    // kIdentity: who he is to the games, his temper and fighting traits for the enemy tactics (docs/ENEMY_TACTICS.md)
    struct Identity
    {
        int Alignment{};    // KARMA -2..2
        uint8_t Injuries{}; // NemesisMan::Injury bits
        bool Legendary{};
        int Temper{-1};     // Nemesis::Personality (Hothead, Coward, Proud, Cold, Greedy, Loyal, Drifter), -1 not sent
        int Wits{1};        // 0 dim, 1 average, 2 sharp
        int Aggression{50}; // 0-100, sent in tens
        int Nerve{50};
        int Loyalty{50};
    };

    // Value: alignment + 2 in bits 0-3, temper + 1 in bits 4-7, injuries in bits 8-15, legendary in bit 16, wits + 1 in
    // bits 17-18, aggression, nerve and loyalty in tens in bits 19-22, 23-26 and 27-30
    [[nodiscard]] static int32_t PackIdentity(const Identity& acIdentity) noexcept
    {
        const auto tens = [](int aValue) { return static_cast<uint32_t>((std::clamp(aValue, 0, 100) + 5) / 10); };
        uint32_t value = static_cast<uint32_t>(std::clamp(acIdentity.Alignment, -2, 2) + 2);
        value |= static_cast<uint32_t>(std::clamp(acIdentity.Temper, -1, 14) + 1) << 4;
        value |= static_cast<uint32_t>(acIdentity.Injuries) << 8;
        value |= acIdentity.Legendary ? 1u << 16 : 0u;
        value |= static_cast<uint32_t>(std::clamp(acIdentity.Wits, 0, 2) + 1) << 17;
        value |= tens(acIdentity.Aggression) << 19 | tens(acIdentity.Nerve) << 23 | tens(acIdentity.Loyalty) << 27;
        return static_cast<int32_t>(value);
    }

    [[nodiscard]] static Identity UnpackIdentity(int32_t aValue) noexcept
    {
        const auto value = static_cast<uint32_t>(aValue);
        Identity identity;
        identity.Alignment = static_cast<int>(value & 0xF) - 2;
        identity.Temper = static_cast<int>((value >> 4) & 0xF) - 1;
        identity.Injuries = static_cast<uint8_t>(value >> 8);
        identity.Legendary = (value >> 16) & 1;
        // From a server that doesn't send them: the defaults
        if (const uint32_t wits = (value >> 17) & 3)
        {
            identity.Wits = static_cast<int>(wits) - 1;
            identity.Aggression = static_cast<int>((value >> 19) & 0xF) * 10;
            identity.Nerve = static_cast<int>((value >> 23) & 0xF) * 10;
            identity.Loyalty = static_cast<int>((value >> 27) & 0xF) * 10;
        }
        return identity;
    }

    NotifyNemesis()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyNemesis& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && Kind == acRhs.Kind && Value == acRhs.Value && RecordId == acRhs.RecordId &&
               ActorServerId == acRhs.ActorServerId && Text == acRhs.Text && Text2 == acRhs.Text2 && Spawn == acRhs.Spawn;
    }

    uint8_t Action{};
    uint8_t Kind{};
    int32_t Value{};
    uint32_t RecordId{};
    uint32_t ActorServerId{};
    String Text{};
    String Text2{};
    NemesisSpawn Spawn{}; // kSpawn only
};
