#include <Messages/RequestSettlementRatings.h>

void RequestSettlementRatings::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, Full);
    Serialization::WriteVarInt(aWriter, Settlements.size());
    for (const SettlementRatings& settlement : Settlements)
        settlement.Serialize(aWriter);
}

void RequestSettlementRatings::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Full = Serialization::ReadBool(aReader);
    // Claimed settlements and C.A.M.P.s: a few dozen
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 1024);
    Settlements.resize(count);
    for (SettlementRatings& settlement : Settlements)
        settlement.Deserialize(aReader);
}
