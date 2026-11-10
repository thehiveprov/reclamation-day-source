#include <Structs/SharedQuest.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void SharedQuest::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    QuestId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Faction);
    Serialization::WriteBool(aWriter, Update);
    Serialization::WriteVarInt(aWriter, Fills.size());
    for (const Fill& fill : Fills)
    {
        Serialization::WriteVarInt(aWriter, fill.AliasId);
        Serialization::WriteVarInt(aWriter, fill.Type);
        fill.FormId.Serialize(aWriter);
        Serialization::WriteVarInt(aWriter, fill.ServerId);
        Serialization::WriteBool(aWriter, fill.Character);
    }
}

void SharedQuest::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    QuestId.Deserialize(aReader);
    Faction = Serialization::ReadVarInt(aReader) & 0xFF;
    Update = Serialization::ReadBool(aReader);
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), kMaxFills);
    Fills.resize(count);
    for (Fill& fill : Fills)
    {
        fill.AliasId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        fill.Type = Serialization::ReadVarInt(aReader) & 0xFF;
        fill.FormId.Deserialize(aReader);
        fill.ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        fill.Character = Serialization::ReadBool(aReader);
    }
}
