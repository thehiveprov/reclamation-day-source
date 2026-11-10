#include <Messages/NotifySettlementProduction.h>

void NotifySettlementProduction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorkshopId.Serialize(aWriter);
    Items.Serialize(aWriter);
}

void NotifySettlementProduction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    WorkshopId.Deserialize(aReader);
    Items.Deserialize(aReader);
}
