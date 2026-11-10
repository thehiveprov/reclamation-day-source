#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "StringCache.h"
#include "Messages/StringCacheUpdate.h"

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/Vector2_NetQuantize.h>

#include <TiltedCore/Math.hpp>
#include <TiltedCore/Platform.hpp>

using namespace TiltedPhoques;

TEST_CASE("Encoding factory", "[encoding.factory]")
{
    Buffer buff(1000);

    {
        AuthenticationRequest request;
        request.Token = "TesSt";

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<AuthenticationRequest>(std::move(pMessage));
        REQUIRE(pRequest->Token == request.Token);
    }

    {
        PartyAcceptInviteRequest request;
        request.InviterId = 123456;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<PartyAcceptInviteRequest>(std::move(pMessage));
        REQUIRE(pRequest->InviterId == request.InviterId);
    }
}

TEST_CASE("Static structures", "[encoding.static]")
{
    GIVEN("GameId")
    {
        GameId sendObjects, recvObjects;
        sendObjects.ModId = 1456987;
        sendObjects.BaseId = 0x789654;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector3_NetQuantize")
    {
        Vector3_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 142.56f;
        sendObjects.y = 45687.7f;
        sendObjects.z = -142.56f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector2_NetQuantize")
    {
        Vector2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1000.89f;
        sendObjects.y = -485632.75f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize")
    {
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1.89f;
        sendObjects.y = TiltedPhoques::Pi * 2.0f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize needing wrap")
    {
        // This test is a bit dangerous as floating errors can lead to sendObjects != recvObjects but the difference is minuscule so we don't care abut such cases
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = -1.87f;
        sendObjects.y = static_cast<float>(TiltedPhoques::Pi) * 18.0f + 3.6f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }
}

TEST_CASE("Differential structures", "[encoding.differential]")
{
    GIVEN("Full ActionEvent")
    {
        ActionEvent sendAction, recvAction;

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("A single cached event name")
    {
        ActionEvent sendAction, recvAction;

        TP_UNUSED(StringCache::Get().Add("test"))

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("Full Mods")
    {
        Mods sendMods, recvMods;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);

        sendMods.ModList.push_back({"Hello", 42});
        sendMods.ModList.push_back({"Hi", 14});
        sendMods.ModList.push_back({"Test", 8});
        sendMods.ModList.push_back({"Toast", 49});

        sendMods.Serialize(writer);

        Buffer::Reader reader(&buff);
        recvMods.Deserialize(reader);

        REQUIRE(sendMods == recvMods);
    }

    GIVEN("AnimationVariables")
    {
        AnimationVariables vars, recvVars;
        vars.Booleans = 0x12345678ull;
        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        Buffer buff(1000);
        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }

        vars.Booleans = 0x9456123ull;
        vars.Floats[3] = 42.f;
        vars.Integers[0] = 18;
        vars.Integers[3] = 0;

        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }
    }
}

TEST_CASE("Packets", "[encoding.packets]")
{
    SECTION("AuthenticationRequest")
    {
        Buffer buff(1000);

        AuthenticationRequest sendMessage, recvMessage;
        sendMessage.Token = "TesSt";
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AuthenticationResponse")
    {
        Buffer buff(1000);

        AuthenticationResponse sendMessage, recvMessage;
        sendMessage.Type = AuthenticationResponse::ResponseType::kAccepted;
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }


    SECTION("AssignCharacterRequest")
    {
        Buffer buff(1000);

        ActionEvent sendAction;
        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        AssignCharacterRequest sendMessage, recvMessage;
        sendMessage.Cookie = 14523698;
        sendMessage.AppearanceBuffer = "toto";
        sendMessage.CellId.BaseId = 45;
        sendMessage.FormId.ModId = 48;
        sendMessage.ReferenceId.BaseId = 456799;
        sendMessage.ReferenceId.ModId = 4079;
        sendMessage.LatestAction = sendAction;
        sendMessage.Position.x = -452.4f;
        sendMessage.Position.y = 452.4f;
        sendMessage.Position.z = 125452.4f;
        sendMessage.Rotation.x = -1.87f;
        sendMessage.Rotation.y = 45.35f;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("ClientReferencesMoveRequest")
    {
        ClientReferencesMoveRequest sendMessage, recvMessage;
        auto& update = sendMessage.Updates[1];
        auto& move = update.UpdatedMovement;

        AnimationVariables vars;
        vars.Booleans = 0x12345678ull;
        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        move.Variables = vars;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(recvMessage.Updates[1].UpdatedMovement == sendMessage.Updates[1].UpdatedMovement);
    }
}

TEST_CASE("StringCache", "[encoding.string_cache]")
{
    SECTION("Messages")
    {
        StringCacheUpdate update;
        update.Values.push_back("Hello");
        update.Values.push_back("Bye");

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        update.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        StringCacheUpdate recvUpdate;
        recvUpdate.DeserializeRaw(reader);

        REQUIRE(update == recvUpdate);
    }
}

TEST_CASE("C.A.M.P.s", "[encoding.camps]")
{
    SECTION("RequestCamp: pick up")
    {
        Buffer buff(1000);

        RequestCamp sendMessage, recvMessage;
        sendMessage.Action = RequestCamp::kPickUp;
        sendMessage.CampId = 7;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("NotifyCamps: one standing, one stored")
    {
        Buffer buff(1000);

        NotifyCamps sendMessage, recvMessage;
        Camp standing;
        standing.Id = 1;
        standing.OwnerName = "Alex";
        standing.ModuleItemId = 42;
        standing.WorldSpaceId = GameId(0, 0x3C);
        standing.Position = {-79048.f, 89587.f, 7846.f};
        standing.Heading = 1.5f;
        Camp stored = standing;
        stored.Id = 2;
        stored.OwnerName = "Robin";
        stored.ModuleItemId = 0;
        stored.Stored = true;
        sendMessage.Camps.push_back(standing);
        sendMessage.Camps.push_back(stored);

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.Camps[1].Stored);
        REQUIRE_FALSE(recvMessage.Camps[0].Stored);
    }
}

TEST_CASE("Trades with another game's character", "[encoding.actor_trade]")
{
    // A modded pipe pistol given and put on, ten 10mm rounds taken back, the old armor taken off
    Inventory::Entry pistol{};
    pistol.BaseId = GameId(0, 0x24F55);
    pistol.Count = 1;
    pistol.ObjectMods.push_back({GameId(0, 0x22F4C), 0, 1});

    Inventory::Entry ammo{};
    ammo.BaseId = GameId(0, 0x1F276);
    ammo.Count = -10;

    Inventory::Entry armor{};
    armor.BaseId = GameId(0, 0x18E3F);
    armor.Count = -1;

    SECTION("RequestActorTrade")
    {
        Buffer buff(1000);

        RequestActorTrade sendMessage, recvMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.OwnershipEpoch = 3;
        sendMessage.Changes.Entries = {pistol, ammo};
        sendMessage.Worn.Entries = {pistol, armor};

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.Changes.Entries[1].Count == -10);
        REQUIRE(recvMessage.Worn.Entries[1].Count == -1);
        REQUIRE(recvMessage.Worn.Entries[0].ObjectMods.size() == 1);
    }

    SECTION("NotifyActorTrade")
    {
        Buffer buff(1000);

        NotifyActorTrade sendMessage, recvMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.OwnershipEpoch = 3;
        sendMessage.Changes.Entries = {pistol, ammo};
        sendMessage.Worn.Entries = {pistol};

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }
}

TEST_CASE("Terminal menu items run for the players nearby", "[encoding.terminal_item_run]")
{
    SECTION("RequestTerminalItemRun")
    {
        Buffer buff(1000);

        RequestTerminalItemRun sendMessage, recvMessage;
        sendMessage.TerminalRefId = GameId(0, 0x1F2A3);
        sendMessage.TerminalFormId = GameId(0, 0x108B54);
        sendMessage.MenuItemId = 4;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("NotifyTerminalItemRun")
    {
        Buffer buff(1000);

        NotifyTerminalItemRun sendMessage, recvMessage;
        sendMessage.PlayerId = 0x2A;
        sendMessage.TerminalRefId = GameId(3, 0x800);
        sendMessage.TerminalFormId = GameId(0, 0x18BC4A);
        sendMessage.MenuItemId = 0xFFFF;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }
}

TEST_CASE("Destruction stages reach the players nearby", "[encoding.destruction_stage]")
{
    SECTION("RequestDestructionStage")
    {
        Buffer buff(1000);

        RequestDestructionStage sendMessage, recvMessage;
        sendMessage.ObjectId = GameId(0, 0x1F2A3);
        sendMessage.Stage = 3;
        sendMessage.Health = 12.5f;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("NotifyDestructionStage")
    {
        Buffer buff(1000);

        NotifyDestructionStage sendMessage, recvMessage;
        sendMessage.PlayerId = 0x2A;
        sendMessage.ObjectId = GameId(3, 0x800);
        sendMessage.Stage = 4;
        sendMessage.Health = 0.f;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }
}

TEST_CASE("Pip-Boy lights reach the other players", "[encoding.pipboy_light]")
{
    SECTION("RequestPipboyLight")
    {
        Buffer buff(1000);

        RequestPipboyLight sendMessage, recvMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.On = true;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("NotifyPipboyLight")
    {
        Buffer buff(1000);

        NotifyPipboyLight sendMessage, recvMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.On = false;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }
}

TEST_CASE("A parkour move is the sender's own or an NPC's", "[encoding.movement_action]")
{
    for (const bool isNpc : {false, true})
    {
        Buffer buff(1000);

        RequestMovementAction sendMessage, recvMessage;
        sendMessage.Type = RequestMovementAction::kParkour;
        sendMessage.Parkour.Move = 3;
        sendMessage.Parkour.Heading = 1.5f;
        sendMessage.Parkour.End[2] = 120.f;
        sendMessage.Parkour.Bump = 4.f;
        sendMessage.Parkour.IsNpc = isNpc;
        sendMessage.Parkour.Npc = isNpc ? 0x12345 : 0;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.Parkour.IsNpc == isNpc);
        REQUIRE(recvMessage.Parkour.Npc == sendMessage.Parkour.Npc);
    }
}

TEST_CASE("An NPC's slide says which NPC", "[encoding.movement_action]")
{
    Buffer buff(1000);

    RequestMovementAction sendMessage, recvMessage;
    sendMessage.Type = RequestMovementAction::kNpcSlide;
    sendMessage.Npc = 0x54321;

    Buffer::Writer writer(&buff);
    sendMessage.Serialize(writer);

    Buffer::Reader reader(&buff);

    uint64_t trash;
    reader.ReadBits(trash, 8); // pop opcode

    recvMessage.DeserializeRaw(reader);

    REQUIRE(sendMessage == recvMessage);
    REQUIRE(recvMessage.Npc == 0x54321);
}

TEST_CASE("The host's movement rules reach everyone", "[encoding.movement_rules]")
{
    MovementRules rules{};
    rules.Sliding = false;
    rules.SlideApCost = 0.35f;
    rules.SprintOutOfCombatCost = 0.f;
    rules.SneakingApDrain = false;
    rules.Parkour = true;
    rules.ParkourApCost = 0.05f;
    rules.LandingRoll = false;
    rules.NpcParkour = false;
    rules.NpcTactics = false;
    rules.NpcSlides = false;

    SECTION("RequestMovementRules")
    {
        Buffer buff(1000);

        RequestMovementRules sendMessage, recvMessage;
        sendMessage.Rules = rules;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.Rules.SlideApCost == 0.35f);
        REQUIRE_FALSE(recvMessage.Rules.Sliding);
    }

    SECTION("NotifySettingsChange, with and without the host's rules")
    {
        for (const bool hasRules : {false, true})
        {
            Buffer buff(1000);

            NotifySettingsChange sendMessage, recvMessage;
            sendMessage.Settings.Difficulty = 3;
            sendMessage.Settings.AutoPartyJoin = true;
            sendMessage.Settings.HasMovementRules = hasRules;
            if (hasRules)
                sendMessage.Settings.Movement = rules;

            Buffer::Writer writer(&buff);
            sendMessage.Serialize(writer);

            Buffer::Reader reader(&buff);

            uint64_t trash;
            reader.ReadBits(trash, 8); // pop opcode

            recvMessage.DeserializeRaw(reader);

            REQUIRE(sendMessage == recvMessage);
            REQUIRE(recvMessage.Settings.HasMovementRules == hasRules);
            REQUIRE(recvMessage.Settings.Movement.LandingRoll == !hasRules);
        }
    }
}

TEST_CASE("The host's settlement day reaches everyone", "[encoding.settlement_day]")
{
    SettlementRatings ratings{};
    ratings.WorkshopId = GameId{0, 0x250FE};
    for (int i = 0; i < 90; ++i)
        ratings.Values.push_back(i * 0.5f);

    SECTION("RequestSettlementRatings and NotifySettlementRatings")
    {
        for (const bool empty : {true, false})
        {
            Buffer buff(4000);

            RequestSettlementRatings sendRequest, recvRequest;
            sendRequest.Full = empty;
            if (!empty)
                sendRequest.Settlements.push_back(ratings);

            Buffer::Writer writer(&buff);
            sendRequest.Serialize(writer);
            Buffer::Reader reader(&buff);
            uint64_t trash;
            reader.ReadBits(trash, 8); // pop opcode
            recvRequest.DeserializeRaw(reader);

            REQUIRE(sendRequest == recvRequest);
            REQUIRE(recvRequest.Settlements.size() == (empty ? 0 : 1));

            Buffer notifyBuff(4000);
            NotifySettlementRatings sendNotify, recvNotify;
            sendNotify.HostPlayerId = empty ? 0 : 7;
            sendNotify.Settlements = sendRequest.Settlements;

            Buffer::Writer notifyWriter(&notifyBuff);
            sendNotify.Serialize(notifyWriter);
            Buffer::Reader notifyReader(&notifyBuff);
            notifyReader.ReadBits(trash, 8); // pop opcode
            recvNotify.DeserializeRaw(notifyReader);

            REQUIRE(sendNotify == recvNotify);
            if (!empty)
                REQUIRE(recvNotify.Settlements[0].Values[89] == 44.5f);
        }
    }

    SECTION("RequestSettlementProduction and NotifySettlementProduction")
    {
        Inventory::Entry water{};
        water.BaseId = GameId{0, 0x4835D};
        water.Count = 12;
        Inventory::Entry caps{};
        caps.BaseId = GameId{0, 0xF};
        caps.Count = 50;

        Buffer buff(1000);
        RequestSettlementProduction sendRequest, recvRequest;
        sendRequest.WorkshopId = ratings.WorkshopId;
        sendRequest.Items.Entries = {water, caps};

        Buffer::Writer writer(&buff);
        sendRequest.Serialize(writer);
        Buffer::Reader reader(&buff);
        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode
        recvRequest.DeserializeRaw(reader);
        REQUIRE(sendRequest == recvRequest);

        Buffer notifyBuff(1000);
        NotifySettlementProduction sendNotify, recvNotify;
        sendNotify.WorkshopId = sendRequest.WorkshopId;
        sendNotify.Items = sendRequest.Items;

        Buffer::Writer notifyWriter(&notifyBuff);
        sendNotify.Serialize(notifyWriter);
        Buffer::Reader notifyReader(&notifyBuff);
        notifyReader.ReadBits(trash, 8); // pop opcode
        recvNotify.DeserializeRaw(notifyReader);
        REQUIRE(sendNotify == recvNotify);
        REQUIRE(recvNotify.Items.Entries[1].Count == 50);
    }
}

TEST_CASE("Faction standing reaches everyone", "[encoding.faction_standing]")
{
    FactionStanding standing{};
    standing.Members = FactionStanding::Bit(FactionStanding::kMinutemen) | FactionStanding::Bit(FactionStanding::kGunners);
    standing.Out = FactionStanding::Bit(FactionStanding::kMinutemen);
    standing.Hostile = FactionStanding::Bit(FactionStanding::kMinutemen) | FactionStanding::Bit(FactionStanding::kRaiders);
    standing.Allied = FactionStanding::Bit(FactionStanding::kRailroad);

    REQUIRE_FALSE(standing.IsMember(FactionStanding::kMinutemen)); // put out
    REQUIRE(standing.IsMember(FactionStanding::kGunners));
    REQUIRE(standing.IsHostile(FactionStanding::kRaiders));

    Buffer buff(100);
    RequestFactionStanding sendRequest, recvRequest;
    sendRequest.Standing = standing;
    Buffer::Writer writer(&buff);
    sendRequest.Serialize(writer);
    Buffer::Reader reader(&buff);
    uint64_t trash;
    reader.ReadBits(trash, 8); // pop opcode
    recvRequest.DeserializeRaw(reader);
    REQUIRE(sendRequest == recvRequest);

    Buffer notifyBuff(100);
    NotifyFactionStanding sendNotify, recvNotify;
    sendNotify.PlayerId = 3;
    sendNotify.Standing = standing;
    Buffer::Writer notifyWriter(&notifyBuff);
    sendNotify.Serialize(notifyWriter);
    Buffer::Reader notifyReader(&notifyBuff);
    notifyReader.ReadBits(trash, 8); // pop opcode
    recvNotify.DeserializeRaw(notifyReader);
    REQUIRE(sendNotify == recvNotify);
    REQUIRE(recvNotify.Standing.Hostile == standing.Hostile);
    REQUIRE(recvNotify.Standing.IsAllied(FactionStanding::kRailroad));
}

TEST_CASE("A faction's radiant quest reaches the party", "[encoding.shared_quest]")
{
    SharedQuest quest{};
    quest.QuestId = GameId{0, 0xA1412};
    quest.Faction = FactionStanding::kMinutemen;
    SharedQuest::Fill settlement{};
    settlement.AliasId = 3;
    settlement.Type = SharedQuest::Fill::kLocation;
    settlement.FormId = GameId{0, 0x1F228};
    SharedQuest::Fill settler{};
    settler.AliasId = 7;
    settler.ServerId = 42;
    settler.Character = true;
    SharedQuest::Fill attacker{};
    attacker.AliasId = 9;
    attacker.Type = SharedQuest::Fill::kCollectionEntry;
    attacker.Character = true;
    quest.Fills = {settlement, settler, attacker};
    quest.Update = true;

    Buffer buff(1000);
    RequestSharedQuest sendRequest, recvRequest;
    sendRequest.Quest = quest;
    Buffer::Writer writer(&buff);
    sendRequest.Serialize(writer);
    Buffer::Reader reader(&buff);
    uint64_t trash;
    reader.ReadBits(trash, 8); // pop opcode
    recvRequest.DeserializeRaw(reader);
    REQUIRE(sendRequest == recvRequest);

    Buffer notifyBuff(1000);
    NotifySharedQuest sendNotify, recvNotify;
    sendNotify.StarterId = 5;
    sendNotify.Quest = quest;
    Buffer::Writer notifyWriter(&notifyBuff);
    sendNotify.Serialize(notifyWriter);
    Buffer::Reader notifyReader(&notifyBuff);
    notifyReader.ReadBits(trash, 8); // pop opcode
    recvNotify.DeserializeRaw(notifyReader);
    REQUIRE(sendNotify == recvNotify);
    REQUIRE(recvNotify.Quest.Fills[1].ServerId == 42);
    REQUIRE(recvNotify.Quest.Fills[0].Type == SharedQuest::Fill::kLocation);
    REQUIRE(recvNotify.Quest.Fills[2].Type == SharedQuest::Fill::kCollectionEntry);
    REQUIRE(recvNotify.Quest.Fills[2].Character);
    REQUIRE(recvNotify.Quest.Update);
}
