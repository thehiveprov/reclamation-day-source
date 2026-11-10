#include "RequestDifficulty.h"

void RequestDifficulty::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Difficulty, 8);
}

void RequestDifficulty::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t dest = 0;
    aReader.ReadBits(dest, 8);
    Difficulty = dest & 0xFF;
}
