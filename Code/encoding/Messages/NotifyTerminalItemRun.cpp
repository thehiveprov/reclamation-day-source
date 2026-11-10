#include <Messages/NotifyTerminalItemRun.h>

void NotifyTerminalItemRun::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, PlayerId);
    TerminalRefId.Serialize(aWriter);
    TerminalFormId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, MenuItemId);
}

void NotifyTerminalItemRun::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    TerminalRefId.Deserialize(aReader);
    TerminalFormId.Deserialize(aReader);
    MenuItemId = Serialization::ReadVarInt(aReader) & 0xFFFF;
}
