#pragma once

#include <libdeflate.h>

#include <cstddef>
#include <cstdint>

// The game's zlib (1.2.7) as its inflate() sees it, and a faster way through the common case: a fresh stream whose whole
// compressed data and whole output both fit in the buffers it was handed (an archive's mesh or texture block) is
// decompressed by libdeflate in one go, and the stream is left as zlib leaves it after Z_STREAM_END. Anything else (a
// stream fed in pieces, gzip, a preset dictionary, data libdeflate refuses) is left untouched for zlib.
namespace WholeInflate
{
// zlib's z_stream on 64-bit Windows (uLong is 32 bits there)
struct State;
struct Stream
{
    const uint8_t* NextIn;
    uint32_t AvailIn;
    uint32_t TotalIn;
    uint8_t* NextOut;
    uint32_t AvailOut;
    uint32_t TotalOut;
    const char* Msg;
    State* pState;
    void* Alloc;
    void* Free;
    void* Opaque;
    int32_t DataType;
    uint32_t Adler;
    uint32_t Reserved;
};
static_assert(offsetof(Stream, NextOut) == 0x10 && offsetof(Stream, pState) == 0x28 && offsetof(Stream, DataType) == 0x48 && sizeof(Stream) == 0x58);

// The start of zlib 1.2.7's inflate_state
struct State
{
    int32_t Mode;
    int32_t Last;
    // bit 0 zlib header, bit 1 gzip header (both: either); 0 raw deflate
    int32_t Wrap;
    int32_t HaveDict;
    int32_t Flags;
    uint32_t MaxDistance;
    uint32_t Check;
    uint32_t Total;
};
static_assert(offsetof(State, Check) == 0x18);

// zlib 1.2.7's inflate modes: a fresh stream, a finished one
constexpr int32_t kModeHead = 0;
constexpr int32_t kModeDone = 28;
constexpr int kStreamEnd = 1;
// Z_BLOCK and Z_TREES stop inside the data: never ours
constexpr int kFlushBlock = 5;

enum class Format
{
    kNone,
    kZlib,
    kRaw,
};

// What a stream holds, if it's one we can take whole: nothing read or written yet, no dictionary, not gzip
[[nodiscard]] inline Format Whole(const Stream& acStream, int aFlush) noexcept
{
    const State* pState = acStream.pState;
    if (!pState || aFlush >= kFlushBlock || pState->Mode != kModeHead || pState->HaveDict || acStream.TotalIn || acStream.TotalOut || !acStream.NextIn || !acStream.NextOut ||
        !acStream.AvailIn || !acStream.AvailOut)
        return Format::kNone;

    switch (pState->Wrap)
    {
    case 0: return Format::kRaw;
    case 1: return Format::kZlib;
    case 3: // zlib or gzip, by the header: not gzip's magic is zlib
        return acStream.AvailIn >= 2 && acStream.NextIn[0] == 0x1F && acStream.NextIn[1] == 0x8B ? Format::kNone : Format::kZlib;
    default: return Format::kNone;
    }
}

// Decompresses the stream whole with libdeflate. True: done, the stream as zlib's inflate leaves it on Z_STREAM_END.
// False: nothing was changed, zlib must do it.
[[nodiscard]] inline bool TryWhole(Stream& aStream, int aFlush, libdeflate_decompressor* apDecompressor) noexcept
{
    const Format format = Whole(aStream, aFlush);
    if (format == Format::kNone || !apDecompressor)
        return false;

    size_t used = 0;
    size_t written = 0;
    const libdeflate_result result =
        format == Format::kZlib ? libdeflate_zlib_decompress_ex(apDecompressor, aStream.NextIn, aStream.AvailIn, aStream.NextOut, aStream.AvailOut, &used, &written)
                                : libdeflate_deflate_decompress_ex(apDecompressor, aStream.NextIn, aStream.AvailIn, aStream.NextOut, aStream.AvailOut, &used, &written);
    if (result != LIBDEFLATE_SUCCESS)
        return false;

    State& state = *aStream.pState;
    if (format == Format::kZlib)
    {
        // the Adler-32 the stream ends with (libdeflate has checked it), big-endian
        const uint8_t* pEnd = aStream.NextIn + used;
        const uint32_t adler = (uint32_t(pEnd[-4]) << 24) | (uint32_t(pEnd[-3]) << 16) | (uint32_t(pEnd[-2]) << 8) | uint32_t(pEnd[-1]);
        state.Check = adler;
        aStream.Adler = adler;
    }
    state.Total = static_cast<uint32_t>(written);
    state.Last = 1;
    state.Mode = kModeDone;

    aStream.NextIn += used;
    aStream.AvailIn -= static_cast<uint32_t>(used);
    aStream.TotalIn += static_cast<uint32_t>(used);
    aStream.NextOut += written;
    aStream.AvailOut -= static_cast<uint32_t>(written);
    aStream.TotalOut += static_cast<uint32_t>(written);
    // the last block seen, no bits left over (what zlib reports at the end of byte-aligned data)
    aStream.DataType = 64;
    return true;
}
} // namespace WholeInflate
