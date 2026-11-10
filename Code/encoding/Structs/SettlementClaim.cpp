#include <Structs/SettlementClaim.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void SettlementClaim::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorkshopId.Serialize(aWriter);
    Serialization::WriteString(aWriter, Name);
    Serialization::WriteString(aWriter, LeaderName);
    Serialization::WriteVarInt(aWriter, LeaderPlayerId);
    Serialization::WriteVarInt(aWriter, Rights.size());
    for (const Right& right : Rights)
    {
        Serialization::WriteString(aWriter, right.Name);
        Serialization::WriteBool(aWriter, right.Build);
        Serialization::WriteBool(aWriter, right.Scrap);
    }
}

void SettlementClaim::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    WorkshopId.Deserialize(aReader);
    Name = Serialization::ReadString(aReader);
    LeaderName = Serialization::ReadString(aReader);
    LeaderPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    // A party has at most a handful of players, rights outlive party changes: a few dozen at most
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 256);
    Rights.resize(count);
    for (Right& right : Rights)
    {
        right.Name = Serialization::ReadString(aReader);
        right.Build = Serialization::ReadBool(aReader);
        right.Scrap = Serialization::ReadBool(aReader);
    }
}
