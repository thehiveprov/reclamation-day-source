#include <Messages/NotifySettlementClaims.h>

void NotifySettlementClaims::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Claims.size());
    for (const SettlementClaim& claim : Claims)
        claim.Serialize(aWriter);
    Serialization::WriteString(aWriter, LocalName);
    Serialization::WriteBool(aWriter, IsAdmin);
}

void NotifySettlementClaims::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    // The game has a few dozen settlements, plugins add some: a few hundred at most
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 1024);
    Claims.resize(count);
    for (SettlementClaim& claim : Claims)
        claim.Deserialize(aReader);
    LocalName = Serialization::ReadString(aReader);
    IsAdmin = Serialization::ReadBool(aReader);
}
