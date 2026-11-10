#include <Messages/RequestFactionStanding.h>

void RequestFactionStanding::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Standing.Serialize(aWriter);
}

void RequestFactionStanding::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Standing.Deserialize(aReader);
}
