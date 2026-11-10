#include "MovementRules.h"
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

bool MovementRules::operator==(const MovementRules& acRhs) const noexcept
{
    return Sliding == acRhs.Sliding && SlideApCost == acRhs.SlideApCost && CheaperSprintOutOfCombat == acRhs.CheaperSprintOutOfCombat &&
           SprintOutOfCombatCost == acRhs.SprintOutOfCombatCost && SwimmingApDrain == acRhs.SwimmingApDrain && SneakingApDrain == acRhs.SneakingApDrain &&
           LandingStumble == acRhs.LandingStumble && Parkour == acRhs.Parkour && ParkourApCost == acRhs.ParkourApCost && LandingRoll == acRhs.LandingRoll &&
           NpcParkour == acRhs.NpcParkour && NpcTactics == acRhs.NpcTactics && NpcSlides == acRhs.NpcSlides;
}

void MovementRules::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Sliding);
    Serialization::WriteFloat(aWriter, SlideApCost);
    Serialization::WriteBool(aWriter, CheaperSprintOutOfCombat);
    Serialization::WriteFloat(aWriter, SprintOutOfCombatCost);
    Serialization::WriteBool(aWriter, SwimmingApDrain);
    Serialization::WriteBool(aWriter, SneakingApDrain);
    Serialization::WriteBool(aWriter, LandingStumble);
    Serialization::WriteBool(aWriter, Parkour);
    Serialization::WriteFloat(aWriter, ParkourApCost);
    Serialization::WriteBool(aWriter, LandingRoll);
    Serialization::WriteBool(aWriter, NpcParkour);
    Serialization::WriteBool(aWriter, NpcTactics);
    Serialization::WriteBool(aWriter, NpcSlides);
}

void MovementRules::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Sliding = Serialization::ReadBool(aReader);
    SlideApCost = Serialization::ReadFloat(aReader);
    CheaperSprintOutOfCombat = Serialization::ReadBool(aReader);
    SprintOutOfCombatCost = Serialization::ReadFloat(aReader);
    SwimmingApDrain = Serialization::ReadBool(aReader);
    SneakingApDrain = Serialization::ReadBool(aReader);
    LandingStumble = Serialization::ReadBool(aReader);
    Parkour = Serialization::ReadBool(aReader);
    ParkourApCost = Serialization::ReadFloat(aReader);
    LandingRoll = Serialization::ReadBool(aReader);
    NpcParkour = Serialization::ReadBool(aReader);
    NpcTactics = Serialization::ReadBool(aReader);
    NpcSlides = Serialization::ReadBool(aReader);
}
