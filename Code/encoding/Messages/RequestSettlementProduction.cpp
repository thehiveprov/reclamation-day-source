#include <Messages/RequestSettlementProduction.h>

void RequestSettlementProduction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorkshopId.Serialize(aWriter);
    Items.Serialize(aWriter);
}

void RequestSettlementProduction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    WorkshopId.Deserialize(aReader);
    Items.Deserialize(aReader);
}
