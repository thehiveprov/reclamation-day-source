#include <Messages/RequestSharedQuest.h>

void RequestSharedQuest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Quest.Serialize(aWriter);
}

void RequestSharedQuest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Quest.Deserialize(aReader);
}
