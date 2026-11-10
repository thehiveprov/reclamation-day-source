#include <Messages/NotifySettlementRatings.h>

void NotifySettlementRatings::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, HostPlayerId);
    Serialization::WriteVarInt(aWriter, Settlements.size());
    for (const SettlementRatings& settlement : Settlements)
        settlement.Serialize(aWriter);
}

void NotifySettlementRatings::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    HostPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 1024);
    Settlements.resize(count);
    for (SettlementRatings& settlement : Settlements)
        settlement.Deserialize(aReader);
}
