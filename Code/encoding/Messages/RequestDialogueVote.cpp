#include <Messages/RequestDialogueVote.h>

void RequestDialogueVote::Option::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteString(aWriter, Prompt);
    Serialization::WriteString(aWriter, Line);
    aWriter.WriteBits(Challenge, 8);
    Serialization::WriteBool(aWriter, Enabled);
}

void RequestDialogueVote::Option::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Prompt = Serialization::ReadString(aReader);
    Line = Serialization::ReadString(aReader);
    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Challenge = static_cast<uint8_t>(value);
    Enabled = Serialization::ReadBool(aReader);
}

void RequestDialogueVote::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    Serialization::WriteVarInt(aWriter, VoteId);
    Serialization::WriteVarInt(aWriter, NpcServerId);
    Serialization::WriteString(aWriter, NpcName);
    Serialization::WriteVarInt(aWriter, Options.size());
    for (const Option& option : Options)
        option.Serialize(aWriter);
    aWriter.WriteBits(Choice, 8);
}

void RequestDialogueVote::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value = 0;
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    VoteId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    NpcServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    NpcName = Serialization::ReadString(aReader);
    // XDI lists any number; more than this is no menu anyone reads
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 32);
    Options.resize(count);
    for (Option& option : Options)
        option.Deserialize(aReader);
    aReader.ReadBits(value, 8);
    Choice = static_cast<uint8_t>(value);
}
