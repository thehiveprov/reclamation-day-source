#include <Messages/NotifyCamps.h>

void NotifyCamps::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Camps.size());
    for (const Camp& camp : Camps)
        camp.Serialize(aWriter);
}

void NotifyCamps::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    // One per player
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 1024);
    Camps.resize(count);
    for (Camp& camp : Camps)
        camp.Deserialize(aReader);
}
