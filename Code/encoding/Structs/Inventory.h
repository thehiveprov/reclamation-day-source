#pragma once

#include "MagicEquipment.h"

using TiltedPhoques::Buffer;
using TiltedPhoques::String;
using TiltedPhoques::Vector;

struct Inventory
{
    struct EffectItem
    {
        float Magnitude{};
        int32_t Area{};
        int32_t Duration{};
        float RawCost{};
        GameId EffectId{};

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
    };

    struct EnchantmentData
    {
        bool IsWeapon{};
        Vector<EffectItem> Effects{};
    };

    // Fallout 4 object mod (OMOD) attached to an item instance, e.g. a scope or a suppressor on a weapon.
    struct ObjectMod
    {
        GameId ModId{};
        uint8_t AttachIndex{};
        uint8_t Rank{};

        bool operator==(const ObjectMod& acRhs) const noexcept { return ModId == acRhs.ModId && AttachIndex == acRhs.AttachIndex && Rank == acRhs.Rank; }

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        static void SerializeList(TiltedPhoques::Buffer::Writer& aWriter, const Vector<ObjectMod>& acMods) noexcept;
        static void DeserializeList(TiltedPhoques::Buffer::Reader& aReader, Vector<ObjectMod>& aMods) noexcept;

        // Whether two lists describe the same item variant: the same set of mods. Order, rank and attach index are
        // ignored, they can come back different after the game rebuilds an instance from a mod list.
        static bool SameMods(const Vector<ObjectMod>& acLhs, const Vector<ObjectMod>& acRhs) noexcept;
    };

    struct Entry
    {
        GameId BaseId{};
        int32_t Count{};

        float ExtraCharge{};

        GameId ExtraEnchantId{};
        uint16_t ExtraEnchantCharge{};
        EnchantmentData EnchantData{};

        float ExtraHealth{};

        GameId ExtraPoisonId{};
        uint32_t ExtraPoisonCount{};

        int32_t ExtraSoulLevel{};

        bool ExtraEnchantRemoveUnequip{};
        bool ExtraWorn{};
        bool ExtraWornLeft{};
        bool IsQuestItem{};

        Vector<ObjectMod> ObjectMods{};
        // The name a player gave the item (renamed at a workbench), empty for the game's own name
        String CustomName{};

        bool operator==(const Entry& acRhs) const noexcept;
        bool operator!=(const Entry& acRhs) const noexcept;

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool ContainsExtraData() const noexcept { return !IsExtraDataEquals(Entry{}); }

        bool CanBeMerged(const Entry& acRhs) const noexcept { return BaseId == acRhs.BaseId && IsExtraDataEquals(acRhs); }

        bool IsExtraDataEquals(const Entry& acRhs) const noexcept
        {
            // TODO: the whole server side state thing is very flawed
            // since many of these things can and will change, like poison id or charge
            // or the fact that the enchant id can be temp
            return ExtraCharge == acRhs.ExtraCharge && ExtraEnchantId == acRhs.ExtraEnchantId && ExtraEnchantCharge == acRhs.ExtraEnchantCharge && ExtraEnchantRemoveUnequip == acRhs.ExtraEnchantRemoveUnequip && ExtraHealth == acRhs.ExtraHealth && ExtraPoisonId == acRhs.ExtraPoisonId &&
                   ExtraPoisonCount == acRhs.ExtraPoisonCount && ExtraSoulLevel == acRhs.ExtraSoulLevel && ExtraWorn == acRhs.ExtraWorn && ExtraWornLeft == acRhs.ExtraWornLeft && IsQuestItem == acRhs.IsQuestItem && ObjectMods == acRhs.ObjectMods && CustomName == acRhs.CustomName;
        }

        bool IsWorn() const noexcept { return ExtraWorn || ExtraWornLeft; }
    };

    bool operator==(const Inventory& acRhs) const noexcept;
    bool operator!=(const Inventory& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    std::optional<Entry> GetEntryById(GameId& aItemId) const noexcept;
    int32_t GetEntryCountById(GameId& aItemId) const noexcept;

    void RemoveByFilter(std::function<bool(const Entry&)> aFilter) noexcept;
    void AddOrRemoveEntry(const Entry& acEntry) noexcept;
    void UpdateEquipment(const Inventory& acNewInventory) noexcept;

    Vector<Entry> Entries{};
    MagicEquipment CurrentMagicEquipment{};
};
