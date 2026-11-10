#include <Messages/RequestItemPickedUp.h>

void RequestItemPickedUp::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ItemId);
}

void RequestItemPickedUp::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ItemId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
