#include <Messages/RequestWeatherChange.h>

void RequestWeatherChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorldSpaceId.Serialize(aWriter);
    Id.Serialize(aWriter);
}

void RequestWeatherChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    WorldSpaceId.Deserialize(aReader);
    Id.Deserialize(aReader);
}
