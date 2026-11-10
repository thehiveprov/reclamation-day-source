#include <Messages/NotifyItemPickedUp.h>

void NotifyItemPickedUp::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ItemId);
}

void NotifyItemPickedUp::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ItemId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
