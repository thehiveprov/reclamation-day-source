#include <Messages/RequestDialogueLock.h>

void RequestDialogueLock::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, NpcServerId);
    Serialization::WriteBool(aWriter, Lock);
}

void RequestDialogueLock::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    NpcServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Lock = Serialization::ReadBool(aReader);
}
