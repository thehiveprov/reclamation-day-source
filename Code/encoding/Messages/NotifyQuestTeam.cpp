#include <Messages/NotifyQuestTeam.h>

namespace
{
template <class T> void WriteList(TiltedPhoques::Buffer::Writer& aWriter, const Vector<T>& acList) noexcept
{
    Serialization::WriteVarInt(aWriter, acList.size());
    for (const T value : acList)
        Serialization::WriteVarInt(aWriter, value);
}

template <class T> void ReadList(TiltedPhoques::Buffer::Reader& aReader, Vector<T>& aList) noexcept
{
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 1024);
    aList.resize(count);
    for (auto& value : aList)
        value = static_cast<T>(Serialization::ReadVarInt(aReader));
}
} // namespace

void NotifyQuestTeam::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Type, 8);
    QuestId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, PlayerId);
    WriteList(aWriter, Participants);
    WriteList(aWriter, Helpers);
    WriteList(aWriter, Stages);
}

void NotifyQuestTeam::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Type = static_cast<uint8_t>(value);
    QuestId.Deserialize(aReader);
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ReadList(aReader, Participants);
    ReadList(aReader, Helpers);
    ReadList(aReader, Stages);
}
