#include <Messages/NotifyFactionStanding.h>

void NotifyFactionStanding::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, PlayerId);
    Standing.Serialize(aWriter);
}

void NotifyFactionStanding::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Standing.Deserialize(aReader);
}
