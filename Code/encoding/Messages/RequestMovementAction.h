#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

// The local player did a movement action whose animation and sounds the other players play on their copy of the
// character (Fallout 4: our slides, landing stumbles and parkour moves, which the animation sync doesn't carry; the
// weapon in the left hand, docs/DUAL_WIELD.md; the grappling gun's line, docs/GRAPPLE.md; and emotes, docs/EMOTES.md)
struct RequestMovementAction final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestMovementAction;

    enum Action : uint8_t
    {
        kSlide,
        kLandingStumble,
        kParkour,
        // the weapon in the left hand now (none: an empty Weapon), sent when it changes and every few seconds
        kOffHand,
        // the left hand fired, swung, began a reload or raised or lowered its guard (OffHand.Cue), for the arm on their
        // copy
        kOffHandCue,
        // the grappling gun's line (Grapple): shot, someone hooked (whose game reels them in), reeled back
        kGrapple,
        // an emote (Emote, docs/EMOTES.md): started, a pose held, stopped
        kEmote,
        // an NPC this game runs slid (Npc, its server id): the others play kSlide on their copy of it
        kNpcSlide,
        // the player's motorcycle under them (Bike): many times a second while riding, its stage 0 once they're off
        kBike,
        // the player's motorcycle standing somewhere (Parked), every few seconds to everyone, wherever they are
        kBikeParked,
        // the player's left hand going out to something (Interact): the others play it on their copy of the character
        kInteract,
        // a raider on a motorcycle, an NPC this game runs (Npc, its server id; Bike): the others get kBike for their copy of them
        kNpcBike,
        // what a caravan truck's doing (Truck), from the game that runs its people to the rest of the party
        kTruck,
    };

    enum Cue : uint8_t
    {
        kCueFire,
        kCueSwing,
        kCueReload,
        kCuePowerSwing,
        kCueGuardOn,
        kCueGuardOff,
    };

    // kOffHand / kOffHandCue: the left hand's weapon (its base form and mods, as the inventory sync has them) and,
    // for a cue, what it did
    struct OffHandState
    {
        GameId Weapon{};
        Vector<Inventory::ObjectMod> Mods{};
        uint8_t Cue{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const OffHandState& acRhs) const noexcept { return Weapon == acRhs.Weapon && Mods == acRhs.Mods && Cue == acRhs.Cue; }
    };

    enum GrappleStage : uint8_t
    {
        // the spike flies From the muzzle To where it bites (or its reach, and comes back) at Speed
        kGrappleShot,
        // it hooked Target (a character's server id): the game that runs them reels them toward the shooter
        kGrappleHooked,
        // it comes back onto the gun (and lets go of whoever it hooked)
        kGrappleRetract,
    };

    // kGrapple: the grappling gun's line as the shooter's game runs it (GrappleService)
    struct GrappleLine
    {
        uint8_t Stage{};
        float From[3]{};
        float To[3]{};
        float Speed{};
        uint32_t Target{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const GrappleLine& acRhs) const noexcept
        {
            return Stage == acRhs.Stage && std::equal(std::begin(From), std::end(From), std::begin(acRhs.From)) &&
                   std::equal(std::begin(To), std::end(To), std::begin(acRhs.To)) && Speed == acRhs.Speed && Target == acRhs.Target;
        }
    };

    enum EmoteStage : uint8_t
    {
        // it starts (a pose: its way down)
        kEmoteStart,
        // a pose holds (its loop), after its way down
        kEmoteHold,
        // it ends (a pose: its way up; a gesture: cut short)
        kEmoteStop,
    };
    // kEmote: which emote (its place in EmoteService's list, the same in every game of a build) and how far it is
    struct EmoteCue
    {
        uint8_t Emote{};
        uint8_t Stage{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const EmoteCue& acRhs) const noexcept { return Emote == acRhs.Emote && Stage == acRhs.Stage; }
    };

    // kBike: what draws the motorcycle and its rider on the other players' screens (RDVehicles, Games/Fallout4/BikeLink.h)
    struct BikeState
    {
        uint8_t Stage{};
        int8_t Side{};
        uint8_t Paint{};
        uint8_t Flags{};
        float Heading{};
        float Lean{};
        float Pitch{};
        float Steer{};
        float Suspension{};
        float WheelSpeed{};
        float Rpm{};
        float Throttle{};
        float StageTime{};
        float Stand{};
        float StandGround{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const BikeState& acRhs) const noexcept { return std::memcmp(this, &acRhs, sizeof(BikeState)) == 0; }
    };

    // kBikeParked: where it stands; Shown false takes it away
    struct BikeParked
    {
        bool Shown{};
        uint8_t Paint{};
        float At[3]{};
        float Heading{};
        float Roll{};
        GameId CellOrWorld{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const BikeParked& acRhs) const noexcept
        {
            return Shown == acRhs.Shown && Paint == acRhs.Paint && std::equal(std::begin(At), std::end(At), std::begin(acRhs.At)) && Heading == acRhs.Heading && Roll == acRhs.Roll &&
                   CellOrWorld == acRhs.CellOrWorld;
        }
    };

    // kInteract: Better Interact's hand (BetterInteractService): which reach (a grab, a press, a push), the item a grab takes
    // (none: nothing in the hand), and where what it reaches for is
    struct InteractCue
    {
        uint8_t Kind{};
        GameId Item{};
        bool HasTarget{};
        float Target[3]{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const InteractCue& acRhs) const noexcept
        {
            return Kind == acRhs.Kind && Item == acRhs.Item && HasTarget == acRhs.HasTarget && std::equal(std::begin(Target), std::end(Target), std::begin(acRhs.Target));
        }
    };

    // kTruck: which truck, whether it's stopped at the side of the road (1) and lost (2), and who's trading from it (a server id)
    struct TruckState
    {
        uint8_t Index{};
        uint8_t Flags{};
        uint32_t Trader{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const TruckState& acRhs) const noexcept { return Index == acRhs.Index && Flags == acRhs.Flags && Trader == acRhs.Trader; }
    };

    // kParkour: the move and the way it goes in the world, which the other players' copy of the character replays
    // (ParkourService::Path): where it starts, the way it faces (radians), where the move's animation expects to
    // start and where it ends, and a vault's extra height. Whose: the sender's character, or an NPC the sender's game
    // runs (IsNpc, its server id: docs/NPC_PARKOUR.md)
    struct ParkourPath
    {
        uint8_t Move{};
        float Start[3]{};
        float Heading{};
        float Ideal[3]{};
        float End[3]{};
        float Bump{};
        bool IsNpc{};
        uint32_t Npc{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const ParkourPath& acRhs) const noexcept
        {
            return Move == acRhs.Move && std::equal(std::begin(Start), std::end(Start), std::begin(acRhs.Start)) && Heading == acRhs.Heading &&
                   std::equal(std::begin(Ideal), std::end(Ideal), std::begin(acRhs.Ideal)) && std::equal(std::begin(End), std::end(End), std::begin(acRhs.End)) &&
                   Bump == acRhs.Bump && IsNpc == acRhs.IsNpc && Npc == acRhs.Npc;
        }
    };

    RequestMovementAction()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestMovementAction& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Type == acRhs.Type && Parkour == acRhs.Parkour && OffHand == acRhs.OffHand && Grapple == acRhs.Grapple &&
               Emote == acRhs.Emote && Npc == acRhs.Npc && Bike == acRhs.Bike && Parked == acRhs.Parked && Interact == acRhs.Interact && Truck == acRhs.Truck;
    }

    uint8_t Type{};
    ParkourPath Parkour{};
    OffHandState OffHand{};
    GrappleLine Grapple{};
    EmoteCue Emote{};
    uint32_t Npc{}; // kNpcSlide
    BikeState Bike{};
    BikeParked Parked{};
    InteractCue Interact{};
    TruckState Truck{};
};
