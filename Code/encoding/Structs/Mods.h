#pragma once

#include <cstdint>

using TiltedPhoques::String;
using TiltedPhoques::Vector;

struct Mods
{
    struct Entry
    {
        // In a refusal's list (Id): a plugin the player has as another version than the host's
        static constexpr uint16_t kDifferentVersion = 0xFFFF;

        String Filename;
        uint16_t Id;
        bool IsLite;
        // Which version of the plugin file it is (its size, beginning and end); 0 if unknown
        uint32_t Fingerprint{0};

        bool operator==(const Entry& acRhs) const noexcept { return Filename == acRhs.Filename && Id == acRhs.Id && IsLite == acRhs.IsLite && Fingerprint == acRhs.Fingerprint; }

        bool operator!=(const Entry& acRhs) const noexcept { return !this->operator==(acRhs); }
    };

    Vector<Entry> ModList{};

    Mods() = default;
    ~Mods() = default;

    bool operator==(const Mods& acRhs) const noexcept;
    bool operator!=(const Mods& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
