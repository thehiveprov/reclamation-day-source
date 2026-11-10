#include "BGSObjectInstanceExtra.h"

// NOTE: ids are CommonLibF4 ids + 1, matching how VersionDb::LoadFallout4 reads the address library.

BGSObjectInstanceExtra* BGSObjectInstanceExtra::New() noexcept
{
    POINTER_FALLOUT4(void, s_vtable, 137210);

    auto* pExtra = static_cast<BGSObjectInstanceExtra*>(Memory::Allocate(sizeof(BGSObjectInstanceExtra)));
    std::memset(static_cast<void*>(pExtra), 0, sizeof(BGSObjectInstanceExtra));

    *reinterpret_cast<void**>(pExtra) = s_vtable.Get();
    pExtra->type = ExtraDataType::ObjectInstance;

    return pExtra;
}

std::span<const BGSObjectInstanceExtra::ObjectIndexData> BGSObjectInstanceExtra::GetIndexData() const noexcept
{
    if (!values || !values->buffer)
        return {};

    // One block descriptor (BSTDataBuffer<1>) right after the payload: size in the low 24 bits, block id in the top 8.
    // The mod list is block 0.
    const uint32_t block = *reinterpret_cast<const uint32_t*>(values->buffer + values->size);
    const uint32_t blockSize = block & 0xFFFFFF;
    const uint32_t blockId = block >> 24;

    if (blockId != 0)
        return {};

    return {reinterpret_cast<const ObjectIndexData*>(values->buffer), blockSize / sizeof(ObjectIndexData)};
}

void BGSObjectInstanceExtra::AddMod(TESForm* apMod, uint8_t aAttachIndex, uint8_t aRank) noexcept
{
    TP_THIS_FUNCTION(TAddMod, void, BGSObjectInstanceExtra, TESForm* apMod, uint8_t aAttachIndex, uint8_t aRank, bool aRemoveInvalidMods);
    POINTER_FALLOUT4(TAddMod, s_addMod, 1191758);

    TiltedPhoques::ThisCall(s_addMod, this, apMod, aAttachIndex, aRank, false);
}
