#include <catch2/catch.hpp>

#include <WholeInflate.h>
#include <zlib.h>

#include <cstring>
#include <string>
#include <vector>

// The game's archive blocks decompressed whole by libdeflate (FastDecompression.cpp) must leave the stream exactly as
// zlib's own inflate does
namespace
{
std::vector<uint8_t> SampleData()
{
    std::vector<uint8_t> data;
    uint32_t seed = 12345;
    for (int i = 0; i < 200000; ++i)
    {
        seed = seed * 1103515245u + 12345u;
        // text-like runs and noise, as meshes and textures mix them
        data.push_back((i / 4096) % 2 ? static_cast<uint8_t>(seed >> 24) : static_cast<uint8_t>("Reclamation Day "[i % 16]));
    }
    return data;
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& acData, bool aZlib)
{
    libdeflate_compressor* pCompressor = libdeflate_alloc_compressor(6);
    std::vector<uint8_t> out(libdeflate_zlib_compress_bound(pCompressor, acData.size()));
    const size_t size = aZlib ? libdeflate_zlib_compress(pCompressor, acData.data(), acData.size(), out.data(), out.size())
                              : libdeflate_deflate_compress(pCompressor, acData.data(), acData.size(), out.data(), out.size());
    libdeflate_free_compressor(pCompressor);
    out.resize(size);
    return out;
}

struct Fake
{
    WholeInflate::State State{};
    WholeInflate::Stream Stream{};

    Fake(const std::vector<uint8_t>& acIn, std::vector<uint8_t>& aOut, int32_t aWrap)
    {
        State.Wrap = aWrap;
        State.Mode = WholeInflate::kModeHead;
        Stream.pState = &State;
        Stream.NextIn = acIn.data();
        Stream.AvailIn = static_cast<uint32_t>(acIn.size());
        Stream.NextOut = aOut.data();
        Stream.AvailOut = static_cast<uint32_t>(aOut.size());
        Stream.Adler = 1;
    }
};
} // namespace

TEST_CASE("A whole block decompressed by libdeflate leaves the stream as zlib does", "[inflate]")
{
    const std::vector<uint8_t> data = SampleData();
    libdeflate_decompressor* pDecompressor = libdeflate_alloc_decompressor();

    for (const bool zlibWrapped : {true, false})
    {
        DYNAMIC_SECTION((zlibWrapped ? "zlib" : "raw deflate") << ", with bytes after the stream and room to spare")
        {
            std::vector<uint8_t> in = Compress(data, zlibWrapped);
            in.insert(in.end(), {1, 2, 3, 4, 5, 6, 7});

            // zlib itself
            std::vector<uint8_t> zlibOut(data.size() + 100);
            z_stream z{};
            REQUIRE((zlibWrapped ? inflateInit(&z) : inflateInit2(&z, -15)) == Z_OK);
            z.next_in = in.data();
            z.avail_in = static_cast<uInt>(in.size());
            z.next_out = zlibOut.data();
            z.avail_out = static_cast<uInt>(zlibOut.size());
            REQUIRE(inflate(&z, Z_NO_FLUSH) == Z_STREAM_END);

            std::vector<uint8_t> ourOut(data.size() + 100);
            Fake fake(in, ourOut, zlibWrapped ? 1 : 0);
            REQUIRE(WholeInflate::TryWhole(fake.Stream, 0, pDecompressor));

            REQUIRE(fake.Stream.NextIn - in.data() == z.next_in - in.data());
            REQUIRE(fake.Stream.AvailIn == z.avail_in);
            REQUIRE(fake.Stream.TotalIn == z.total_in);
            REQUIRE(fake.Stream.NextOut - ourOut.data() == z.next_out - zlibOut.data());
            REQUIRE(fake.Stream.AvailOut == z.avail_out);
            REQUIRE(fake.Stream.TotalOut == z.total_out);
            if (zlibWrapped)
                REQUIRE(fake.Stream.Adler == z.adler);
            REQUIRE(fake.State.Mode == WholeInflate::kModeDone);
            REQUIRE(std::memcmp(ourOut.data(), data.data(), data.size()) == 0);
            inflateEnd(&z);
        }
    }

    SECTION("zlib or gzip by the header (wrap 3): a zlib header is taken")
    {
        const std::vector<uint8_t> in = Compress(data, true);
        std::vector<uint8_t> out(data.size());
        Fake fake(in, out, 3);
        REQUIRE(WholeInflate::TryWhole(fake.Stream, 4, pDecompressor));
        REQUIRE(fake.Stream.AvailOut == 0);
        REQUIRE(fake.Stream.TotalOut == data.size());
    }

    libdeflate_free_decompressor(pDecompressor);
}

TEST_CASE("Blocks that aren't whole are left untouched for zlib", "[inflate]")
{
    const std::vector<uint8_t> data = SampleData();
    const std::vector<uint8_t> in = Compress(data, true);
    libdeflate_decompressor* pDecompressor = libdeflate_alloc_decompressor();

    auto untouched = [&](Fake& aFake, const std::vector<uint8_t>& acIn, uint32_t aAvailIn, uint32_t aAvailOut)
    {
        REQUIRE(aFake.Stream.NextIn == acIn.data());
        REQUIRE(aFake.Stream.AvailIn == aAvailIn);
        REQUIRE(aFake.Stream.AvailOut == aAvailOut);
        REQUIRE(aFake.Stream.TotalIn == 0);
        REQUIRE(aFake.Stream.TotalOut == 0);
        REQUIRE(aFake.State.Mode == WholeInflate::kModeHead);
    };

    SECTION("The output doesn't fit (a stream read in pieces)")
    {
        std::vector<uint8_t> out(data.size() - 1);
        Fake fake(in, out, 1);
        REQUIRE_FALSE(WholeInflate::TryWhole(fake.Stream, 0, pDecompressor));
        untouched(fake, in, static_cast<uint32_t>(in.size()), static_cast<uint32_t>(out.size()));
    }

    SECTION("Only part of the input is there yet")
    {
        std::vector<uint8_t> out(data.size());
        Fake fake(in, out, 1);
        fake.Stream.AvailIn = static_cast<uint32_t>(in.size() / 2);
        REQUIRE_FALSE(WholeInflate::TryWhole(fake.Stream, 0, pDecompressor));
        untouched(fake, in, static_cast<uint32_t>(in.size() / 2), static_cast<uint32_t>(out.size()));
    }

    SECTION("gzip, a stream already started, a block-by-block flush, a dictionary")
    {
        std::vector<uint8_t> out(data.size());
        Fake gzip(in, out, 2);
        REQUIRE_FALSE(WholeInflate::TryWhole(gzip.Stream, 0, pDecompressor));

        Fake started(in, out, 1);
        started.State.Mode = 11;
        REQUIRE_FALSE(WholeInflate::TryWhole(started.Stream, 0, pDecompressor));

        Fake block(in, out, 1);
        REQUIRE_FALSE(WholeInflate::TryWhole(block.Stream, 5, pDecompressor));

        Fake dictionary(in, out, 1);
        dictionary.State.HaveDict = 1;
        REQUIRE_FALSE(WholeInflate::TryWhole(dictionary.Stream, 0, pDecompressor));
        untouched(dictionary, in, static_cast<uint32_t>(in.size()), static_cast<uint32_t>(out.size()));
    }

    SECTION("Data that isn't deflate")
    {
        std::vector<uint8_t> junk(in.size(), 0xAB);
        junk[0] = 0x78;
        junk[1] = 0x9C;
        std::vector<uint8_t> out(data.size());
        Fake fake(junk, out, 1);
        REQUIRE_FALSE(WholeInflate::TryWhole(fake.Stream, 0, pDecompressor));
        untouched(fake, junk, static_cast<uint32_t>(junk.size()), static_cast<uint32_t>(out.size()));
    }

    libdeflate_free_decompressor(pDecompressor);
}
