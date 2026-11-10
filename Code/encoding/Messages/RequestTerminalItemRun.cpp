#include <Messages/RequestTerminalItemRun.h>

void RequestTerminalItemRun::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TerminalRefId.Serialize(aWriter);
    TerminalFormId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, MenuItemId);
}

void RequestTerminalItemRun::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    TerminalRefId.Deserialize(aReader);
    TerminalFormId.Deserialize(aReader);
    MenuItemId = Serialization::ReadVarInt(aReader) & 0xFFFF;
}
