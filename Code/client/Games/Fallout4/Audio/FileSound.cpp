#include <Audio/FileSound.h>

#include <Forms/TESForm.h>
#include <TESObjectREFR.h>

namespace
{
// BSISoundDescriptor's interface (CommonLibF4): what the audio manager asks of a descriptor
struct ISoundDescriptor
{
    virtual ~ISoundDescriptor() = default;                                                // 00
    virtual bool DoResolve(void* apResolution, float aDistance, void* apExtra) const = 0; // 01
    virtual bool DoMultiResolve(void* apResolutions, void* apExtra) const = 0;            // 02
    virtual bool DoAudibilityTest(float aDistance) const = 0;                             // 03
    virtual bool DoGetMultiResolves() const = 0;                                          // 04
};

// BSSoundHandle (CommonLibF4)
struct SoundHandle
{
    uint32_t soundId{0xFFFFFFFF};
    bool assumeSuccess{false};
    int8_t state{0};
    uint16_t pad{0};
};

// What Papyrus Sound.Play does (its native, disassembled on 1.10.163): the listener's distance to the source, build the
// sound from the descriptor with these flags, follow the source's 3D, play
constexpr uint32_t kSoundFlags = 0x10;
// How far into the engine's resolution the calibration looks for the template's file
constexpr size_t kResolutionSearchSize = 0x80;

void* GetAudioManager() noexcept
{
    using TGetAudioManager = void*();
    POINTER_FALLOUT4(TGetAudioManager, s_getAudioManager, 679749);
    return s_getAudioManager.Get()();
}

float GetListenerDistance(void* apManager, const NiPoint3& acPosition) noexcept
{
    using TGetListenerDistance = float(void*, const NiPoint3*);
    POINTER_FALLOUT4(TGetListenerDistance, s_getListenerDistance, 1442298);
    return s_getListenerDistance.Get()(apManager, &acPosition);
}

void BuildSoundDataFromDescriptor(void* apManager, SoundHandle& aHandle, const ISoundDescriptor* apDescriptor, float aDistance) noexcept
{
    using TBuildSoundData = void(void*, SoundHandle*, const ISoundDescriptor*, float, uint32_t, void*);
    POINTER_FALLOUT4(TBuildSoundData, s_buildSoundData, 1419046);
    s_buildSoundData.Get()(apManager, &aHandle, apDescriptor, aDistance, kSoundFlags, nullptr);
}

void SetObjectToFollow(SoundHandle& aHandle, NiNode* apNode) noexcept
{
    using TSetObjectToFollow = void(SoundHandle*, NiNode*);
    POINTER_FALLOUT4(TSetObjectToFollow, s_setObjectToFollow, 1179145);
    s_setObjectToFollow.Get()(&aHandle, apNode);
}

bool PlaySound(SoundHandle& aHandle) noexcept
{
    using TPlay = bool(SoundHandle*);
    POINTER_FALLOUT4(TPlay, s_play, 384074);
    return s_play.Get()(&aHandle);
}

std::string ToLower(std::string aText) noexcept
{
    std::ranges::transform(aText, aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
}
} // namespace

struct FileSound::Descriptor final : ISoundDescriptor
{
    const ISoundDescriptor* pTemplate{nullptr};
    std::vector<uint32_t> templateFileHashes;
    uint32_t templateFolderHash{0};
    uint32_t templateDataFolderHash{0};
    std::string fileName;
    std::string folder;

    // found on the first resolve
    mutable bool calibrated{false};
    mutable int32_t idOffset{-1};
    mutable ResourceId ours{};

    bool DoResolve(void* apResolution, float aDistance, void* apExtra) const override
    {
        if (!pTemplate->DoResolve(apResolution, aDistance, apExtra))
            return false;

        auto* pBytes = static_cast<uint8_t*>(apResolution);
        if (!calibrated)
            Calibrate(pBytes);

        // without knowing where the file is, play nothing rather than the template's own sound
        if (idOffset < 0)
            return false;

        std::memcpy(pBytes + idOffset, &ours, sizeof(ours));
        return true;
    }

    bool DoMultiResolve(void* apResolutions, void* apExtra) const override { return pTemplate->DoMultiResolve(apResolutions, apExtra); }
    bool DoAudibilityTest(float aDistance) const override { return pTemplate->DoAudibilityTest(aDistance); }
    bool DoGetMultiResolves() const override { return pTemplate->DoGetMultiResolves(); }

    void Calibrate(const uint8_t* apResolution) const
    {
        calibrated = true;

        for (size_t offset = 0; offset + sizeof(ResourceId) <= kResolutionSearchSize; offset += 4)
        {
            ResourceId id{};
            std::memcpy(&id, apResolution + offset, sizeof(id));
            if (std::ranges::find(templateFileHashes, id.file) == templateFileHashes.end())
                continue;

            // our id in the same form as the template's: the folder with or without Data\, its extension
            const bool withData = id.dir == templateDataFolderHash;
            if (!withData && id.dir != templateFolderHash)
            {
                spdlog::warn("FileSound: template file at +{:#x} but its folder hash {:08X} is neither form", offset, id.dir);
                continue;
            }

            ours.file = Crc(fileName);
            ours.ext = id.ext;
            ours.dir = Crc(withData ? "data\\" + folder : folder);
            idOffset = static_cast<int32_t>(offset);
            spdlog::info("FileSound: file id at +{:#x} (folder {} Data\\, extension {:08X}), playing {}\\{}", offset, withData ? "with" : "without", id.ext, folder, fileName);
            return;
        }

        std::string dump;
        for (size_t i = 0; i < kResolutionSearchSize; ++i)
            dump += fmt::format("{:02X}{}", apResolution[i], (i + 1) % 16 ? " " : " | ");
        spdlog::warn("FileSound: the template's file isn't in its resolution, no sound: {}", dump);
    }
};

FileSound::FileSound(uint32_t aTemplateFormId, std::vector<std::string> aTemplateFiles, std::string aTemplateFolder, std::string aFile) noexcept
    : m_templateFormId(aTemplateFormId)
    , m_pDescriptor(std::make_unique<Descriptor>())
{
    for (const auto& file : aTemplateFiles)
        m_pDescriptor->templateFileHashes.push_back(Crc(ToLower(file)));
    const std::string templateFolder = ToLower(std::move(aTemplateFolder));
    m_pDescriptor->templateFolderHash = Crc(templateFolder);
    m_pDescriptor->templateDataFolderHash = Crc("data\\" + templateFolder);

    const std::string file = ToLower(std::move(aFile));
    const auto slash = file.find_last_of('\\');
    m_pDescriptor->folder = file.substr(0, slash);
    m_pDescriptor->fileName = file.substr(slash + 1);
}

FileSound::~FileSound() noexcept = default;

bool FileSound::Play(TESObjectREFR* apSource) noexcept
{
    if (!apSource)
        return false;

    if (!m_pDescriptor->pTemplate)
    {
        TESForm* pTemplate = TESForm::GetById(m_templateFormId);
        if (!pTemplate)
        {
            spdlog::warn("FileSound: template sound {:X} not found", m_templateFormId);
            return false;
        }
        // BGSSoundDescriptorForm: its BSISoundDescriptor at 0x20
        m_pDescriptor->pTemplate = reinterpret_cast<const ISoundDescriptor*>(reinterpret_cast<uint8_t*>(pTemplate) + 0x20);
    }

    void* pManager = GetAudioManager();
    if (!pManager)
        return false;

    SoundHandle handle{};
    BuildSoundDataFromDescriptor(pManager, handle, m_pDescriptor.get(), GetListenerDistance(pManager, apSource->position));
    if (handle.soundId == 0xFFFFFFFF)
        return false;

    if (NiNode* pNode = apSource->GetNiNode())
        SetObjectToFollow(handle, pNode);
    return PlaySound(handle);
}

bool FileSound::PlayDescriptor(const TESForm* apDescriptor, TESObjectREFR* apSource) noexcept
{
    if (!apDescriptor || !apSource)
        return false;
    void* pManager = GetAudioManager();
    if (!pManager)
        return false;

    // BGSSoundDescriptorForm: its BSISoundDescriptor at 0x20
    const auto* pInterface = reinterpret_cast<const ISoundDescriptor*>(reinterpret_cast<const uint8_t*>(apDescriptor) + 0x20);
    SoundHandle handle{};
    BuildSoundDataFromDescriptor(pManager, handle, pInterface, GetListenerDistance(pManager, apSource->position));
    if (handle.soundId == 0xFFFFFFFF)
        return false;

    if (NiNode* pNode = apSource->GetNiNode())
        SetObjectToFollow(handle, pNode);
    return PlaySound(handle);
}

// CRC32 (reflected 0xEDB88320) with no initial value or final xor, as the game hashes resource names and folders
uint32_t FileSound::Crc(std::string_view aText) noexcept
{
    static const auto s_table = []
    {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return table;
    }();

    uint32_t crc = 0;
    for (const unsigned char c : aText)
        crc = s_table[(crc ^ c) & 0xFF] ^ (crc >> 8);
    return crc;
}
