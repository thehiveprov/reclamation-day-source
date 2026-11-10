#include <Messages/NotifyDialogueVote.h>

void NotifyDialogueVote::Voter::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, PlayerId);
    Serialization::WriteString(aWriter, Name);
    aWriter.WriteBits(static_cast<uint8_t>(Choice), 8);
}

void NotifyDialogueVote::Voter::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    PlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Name = Serialization::ReadString(aReader);
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Choice = static_cast<int8_t>(static_cast<uint8_t>(value));
}

void NotifyDialogueVote::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Stage, 8);
    Serialization::WriteVarInt(aWriter, VoteId);
    Serialization::WriteVarInt(aWriter, TalkerId);
    Serialization::WriteVarInt(aWriter, NpcServerId);
    Serialization::WriteString(aWriter, NpcName);
    Serialization::WriteVarInt(aWriter, Options.size());
    for (const auto& option : Options)
        option.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Voters.size());
    for (const Voter& voter : Voters)
        voter.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, MillisecondsLeft);
    aWriter.WriteBits(static_cast<uint8_t>(Result), 8);
    Serialization::WriteBool(aWriter, HostBrokeTie);
}

void NotifyDialogueVote::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Stage = static_cast<uint8_t>(value);
    VoteId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    TalkerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    NpcServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    NpcName = Serialization::ReadString(aReader);
    const auto optionCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 32);
    Options.resize(optionCount);
    for (auto& option : Options)
        option.Deserialize(aReader);
    // A party
    const auto voterCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 64);
    Voters.resize(voterCount);
    for (Voter& voter : Voters)
        voter.Deserialize(aReader);
    MillisecondsLeft = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    aReader.ReadBits(value, 8);
    Result = static_cast<int8_t>(static_cast<uint8_t>(value));
    HostBrokeTie = Serialization::ReadBool(aReader);
}
