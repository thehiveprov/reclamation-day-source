#include <Structs/SettlementRatings.h>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

void SettlementRatings::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorkshopId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Values.size());
    for (const float value : Values)
        Serialization::WriteFloat(aWriter, value);
}

void SettlementRatings::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    WorkshopId.Deserialize(aReader);
    const auto count = std::min<uint64_t>(Serialization::ReadVarInt(aReader), kMaxValues);
    Values.resize(count);
    for (float& value : Values)
        value = Serialization::ReadFloat(aReader);
}
