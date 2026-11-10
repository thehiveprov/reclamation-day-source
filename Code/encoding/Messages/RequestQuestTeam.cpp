#include <Messages/RequestQuestTeam.h>

void RequestQuestTeam::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    QuestId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, PlayerId);
    aWriter.WriteBits(QuestProgress, 8);
    Serialization::WriteVarInt(aWriter, Stages.size());
    for (const uint16_t stage : Stages)
        Serialization::WriteVarInt(aWriter, stage);
}

void RequestQuestTeam::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    QuestId.Deserialize(aReader);
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    aReader.ReadBits(value, 8);
    QuestProgress = static_cast<uint8_t>(value);

    // A quest has at most a few hundred stages
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 1024);
    Stages.resize(count);
    for (auto& stage : Stages)
        stage = Serialization::ReadVarInt(aReader) & 0xFFFF;
}
