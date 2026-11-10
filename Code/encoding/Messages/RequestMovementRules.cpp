#include "RequestMovementRules.h"

void RequestMovementRules::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Rules.Serialize(aWriter);
}

void RequestMovementRules::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Rules.Deserialize(aReader);
}
