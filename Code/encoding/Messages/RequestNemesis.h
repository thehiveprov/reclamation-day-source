#pragma once

#include "Message.h"

using TiltedPhoques::String;
using TiltedPhoques::Vector;

// Nemeses (Fallout 4, see docs/NEMESIS.md): what the local player's game tells the server. The server keeps the
// nemesis world and decides; games report deeds and deaths, spawn what they're sent, and ask for the overlay's data.
// Places are worldspace keys ("Fallout4.esm:3C", the plugin and the form's id in it), "" inside.
struct RequestNemesis final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestNemesis;

    enum Action : uint8_t
    {
        kHello,    // on connecting and on load: Text = campaign ("2287", "winter-of-atom"), Value = karma, Kind = alignment 0-4,
                   // Text2 = the plugins that bring gangs (DLCs, KARMA.esm, RD_WinterOfAtom.esp), comma separated
        kDeed,     // Kind = Deed, by or against the local player, on a nemesis (RecordId) or a stranger (see below)
        kDeath,    // a nemesis (RecordId) or a gang man (a stranger) died near the local player: Kind = Cause, Value = DeathFlags
        kOutcome,  // the local player's encounter ended: RecordId = encounter id, Kind = Outcome
        kSpawned,  // the encounter is in the world: RecordId = encounter id, ServerIds = the men's server ids (0: not spawned)
        kKarma,    // KARMA changed: Value = karma, Kind = alignment 0-4
        kUi,       // the overlay: Text = request, Text2 = payload
        kPlace,    // the local player entered a location: Text = its key ("Fallout4.esm:24F93")
        kQuestion, // questioning a surrendered man: RecordId (0: stranger by ActorServerId), Kind = Question, Value = caps (kPay)
        kStatus,   // every few seconds: Kind = 1 if the local player can be met (outside, not busy, not at home), World + Position
        kLook,     // a man's face as his game has it: RecordId (0: by ActorServerId), Appearance + AppearanceFlags
        kWound,    // the local player's hits on a man within a moment (the wound log): RecordId (0: a stranger, as a deed's),
                   // Kind = WoundPart (| kPartCrippled when the limb gave out), Body = Cause, Value = damage, Count = hits,
                   // Range = how far (game units), Text = the weapon's name, Text2 = its ammo's, World + Position = where he was
    };

    // Where hits landed, by the limb whose condition they took (Nemesis::Part)
    enum WoundPart : uint8_t
    {
        kPartHead,
        kPartTorso,
        kPartLeftArm,
        kPartRightArm,
        kPartLeftLeg,
        kPartRightLeg,
        kPartUnknown,
        kPartCrippled = 0x80,
    };

    // Deeds. A stranger is described by ActorServerId, Body (NemesisMan::Body), Appearance, Text = gang key,
    // Value = amount, World + Position, Text2 = the place's name.
    enum Deed : uint8_t
    {
        kDownedYou,     // he put the local player down
        kWounded,       // the local player took half his health or more and he lived through the fight
        kCrippled,      // the local player crippled one of his limbs (Value: 1 a leg, 2 an arm) and he lived
        kComradeKilled, // the local player killed a man of his gang in front of him and he lived
        kRobbed,        // the local player picked his pocket
        kGunfight,      // he was in a fight with the local player and lived, unhurt
        kSurrendered,   // he surrendered to the local player
        kLetGo,         // the local player let a surrendered man go (karma +)
        kExecuted,      // the local player killed a surrendered or downed man (karma -)
        kThreatened,    // the local player threatened a surrendered man (questioning)
        kPaid,          // the local player paid a surrendered man (Value: caps)
        kHit,           // the local player's hits on him in a fight (Value: damage), towards a stranger's grudge score
        kStashRobbed,   // the local player emptied a gang's stash (Text: gang key)
        kCampCleared,   // the local player cleared a location a gang held (Text: gang key, Text2: the location's key)
        kMet,           // the local player saw a nemesis' face (his name card), or a KARMA bounty hunter (Text: gang key)
    };

    enum Cause : uint8_t
    {
        kGunshot,
        kBlade,
        kBlunt,
        kFire,
        kEnergy,
        kExplosion,
        kClaws,
        kOther,
    };

    enum DeathFlags : int32_t
    {
        kByLocalPlayer = 1 << 0,
        kFinal = 1 << 1,        // a headshot kill or dismembered: no cheating death
        kWhileSurrendered = 1 << 2,
        kByOtherPlayer = 1 << 3,
        kLeftLeg = 1 << 4,      // limbs crippled when he died (scars)
        kRightLeg = 1 << 5,
        kLeftArm = 1 << 6,
        kRightArm = 1 << 7,
        kHead = 1 << 8,
    };

    enum Outcome : uint8_t
    {
        kHeKilled,     // reported by kDeath; the encounter's end
        kHeDownedYou,  // the player went down
        kHeFled,       // he got away (out of range alive)
        kHeSurrendered,
        kYouLeft,      // the player left before it started (too far, a load)
        kBackedDown,   // a call-out talked down
        kCancelled,    // couldn't spawn
    };

    enum Question : uint8_t
    {
        kAskBoss,
        kAskCamp,
        kAskStash,
        kAskKin,
        kAskHabits,
        kAskWhy,      // "give me one reason": he bargains
        kThreaten,
        kPay,
        kLetHimGo,
        kFinishHim,
        kClose,
        kOpen,        // the card, nothing asked yet
    };

    RequestNemesis()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestNemesis& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && Kind == acRhs.Kind && Value == acRhs.Value && RecordId == acRhs.RecordId &&
               ActorServerId == acRhs.ActorServerId && Body == acRhs.Body && World == acRhs.World && Position == acRhs.Position && Text == acRhs.Text &&
               Text2 == acRhs.Text2 && ServerIds == acRhs.ServerIds && Appearance == acRhs.Appearance && AppearanceFlags == acRhs.AppearanceFlags &&
               Range == acRhs.Range && Count == acRhs.Count;
    }

    uint8_t Action{};
    uint8_t Kind{};
    int32_t Value{};
    uint32_t RecordId{};
    uint32_t ActorServerId{};
    uint8_t Body{};
    String World{};
    std::array<float, 3> Position{};
    String Text{};
    String Text2{};
    Vector<uint32_t> ServerIds{};
    String Appearance{};
    uint32_t AppearanceFlags{};
    float Range{};    // kWound
    uint16_t Count{}; // kWound
};
