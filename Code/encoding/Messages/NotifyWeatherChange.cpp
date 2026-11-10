#include <Messages/NotifyWeatherChange.h>

void NotifyWeatherChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorldSpaceId.Serialize(aWriter);
    Id.Serialize(aWriter);
    Serialization::WriteBool(aWriter, Lead);
}

void NotifyWeatherChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    WorldSpaceId.Deserialize(aReader);
    Id.Deserialize(aReader);
    Lead = Serialization::ReadBool(aReader);
}
