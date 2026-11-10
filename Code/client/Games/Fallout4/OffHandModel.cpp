#include <HivemindPCH.h>

#include <OffHandModel.h>
#include <NodeTools.h>

#include <World.h>
#include <Games/TES.h>
#include <Forms/TESForm.h>
#include <ExtraData/ExtraDataList.h>
#include <ExtraData/BGSObjectInstanceExtra.h>
#include <Misc/BSFixedString.h>

namespace
{
// The reference the look is built from: this far under where it's placed (out of sight; it falls until it's gone), and
// given up on when its 3D isn't there after this long (s)
constexpr float kBelow = 2000.f;
constexpr float kGiveUpAfter = 4.f;

size_t CountNodes(uint8_t* apNode) noexcept
{
    size_t count = 1;
    NodeTools::ForEachChild(apNode, [&count](uint8_t* apChild) { count += CountNodes(apChild); });
    return count;
}

// The copy's nodes of the skeleton's names renamed (OffHandModel::kModelName), how many
size_t RenameSkeletonNames(uint8_t* apNode) noexcept
{
    size_t renamed = 0;
    const std::string name = NodeTools::GetName(apNode);
    if (_stricmp(name.c_str(), "WEAPON") == 0 || _stricmp(name.c_str(), "WeaponLeft") == 0)
    {
        NodeTools::SetName(apNode, fmt::format("{}{}", OffHandModel::kModelName, name).c_str());
        ++renamed;
    }
    NodeTools::ForEachChild(apNode, [&renamed](uint8_t* apChild) { renamed += RenameSkeletonNames(apChild); });
    return renamed;
}

int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

OffHandModel::~OffHandModel() noexcept
{
    Reset();
}

void OffHandModel::Build(TESForm* apWeapon, const Vector<OffHandMod>& acMods, TESObjectREFR* apNear) noexcept
{
    Reset();
    if (!apWeapon || !apNear || !apNear->parentCell)
        return;
    m_weaponId = apWeapon->formID;

    // Made with its mods, as the stack has them, the way the game places a modded object (CreateReferenceAtLocation's
    // modExtra: on the reference before its 3D is built, attachments and all). Seen 2026-10-25: the left weapons showed
    // none of their attachments when the mods went on after it was placed (its plain 3D was there a frame later, before a
    // disable and enable could make it again)
    BGSObjectInstanceExtra* pInstance = nullptr;
    if (!acMods.empty())
    {
        pInstance = BGSObjectInstanceExtra::New();
        for (const OffHandMod& mod : acMods)
        {
            if (TESForm* pMod = TESForm::GetById(mod.FormId))
                pInstance->AddMod(pMod, mod.AttachIndex, mod.Rank);
        }
    }
    TESWorldSpace* pWorldSpace = apNear->GetWorldSpace();
    const NiPoint3 at(glm::vec3(apNear->position) - glm::vec3(0.f, 0.f, kBelow));
    const uint32_t handle = ModManager::Get()->SpawnObject(apWeapon, at, NiPoint3(), pWorldSpace ? nullptr : apNear->parentCell, pWorldSpace, pInstance);
    TESObjectREFR* pRef = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
    if (!pRef)
    {
        spdlog::warn("Dual wield: couldn't place {:X} to build its look", apWeapon->formID);
        return;
    }
    // what its mods make of it too (its materials and the rest: the game works that out only for an item it gave its own
    // mods, as TESObjectREFR.cpp's inventory adds do)
    if (ExtraDataList* pExtra = pInstance ? pRef->GetExtraDataList() : nullptr)
        pExtra->CreateInstanceData(static_cast<TESBoundObject*>(apWeapon));

    m_buildRefId = pRef->formID;
    m_buildTime = static_cast<float>(NowMs()) / 1000.f;
    spdlog::info("Dual wield: building the look of {:X} ({} mods) from {:X}", apWeapon->formID, acMods.size(), pRef->formID);
}

void OffHandModel::Update() noexcept
{
    // the flash put away once its time is up
    if (m_flashUntil && NowMs() >= m_flashUntil)
    {
        m_flashUntil = 0;
        for (auto& attached : m_attached)
        {
            if (attached.pFlash)
                NodeTools::SetCulled(attached.pFlash, true);
        }
    }

    if (!m_buildRefId)
        return;
    auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(m_buildRefId));
    uint8_t* p3D = pRef ? NodeTools::GetThirdPersonRoot(pRef) : nullptr;
    const float waited = static_cast<float>(NowMs()) / 1000.f - m_buildTime;
    if (!p3D)
    {
        if (waited > kGiveUpAfter)
        {
            spdlog::warn("Dual wield: the look of {:X} never loaded", m_weaponId);
            DropBuildReference();
        }
        return;
    }

    m_pSource = NodeTools::Clone(p3D);
    if (m_pSource)
    {
        NodeTools::IncRef(m_pSource);
        // at the hand's own node, not where the reference was
        NodeTools::Write(m_pSource, NodeTools::kLocalOffset, NodeTools::Transform{});
        // (named as no skeleton node is: the baton's root is WEAPON, the right hand's weapon node's name, and the baton
        // sat away from the left hand, 2026-10-25)
        const std::string rootName = NodeTools::GetName(m_pSource);
        const size_t renamed = RenameSkeletonNames(m_pSource);
        NodeTools::SetName(m_pSource, kModelName);
        spdlog::info("Dual wield: the look of {:X} is ready ({} nodes, after {:.2f} s; its root was \"{}\", {} of the skeleton's names renamed)", m_weaponId, CountNodes(m_pSource), waited,
                     rootName, renamed);
    }
    DropBuildReference();
}

void OffHandModel::DropBuildReference() noexcept
{
    if (auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(m_buildRefId)))
    {
        pRef->Disable();
        pRef->Delete();
    }
    m_buildRefId = 0;
}

NodeTools::Transform OffHandModel::OnWeaponLeft() noexcept
{
    NodeTools::Transform turned{};
    turned.Rows[0] = glm::vec3(-1.f, 0.f, 0.f);
    turned.Rows[2] = glm::vec3(0.f, 0.f, -1.f);
    return turned;
}

void OffHandModel::AttachTo(uint8_t* apRoot) noexcept
{
    static BSFixedString s_weaponLeft("WeaponLeft");
    if (!m_pSource || !apRoot)
        return;
    uint8_t* pHand = NodeTools::GetObjectByName(apRoot, s_weaponLeft);
    for (auto& attached : m_attached)
    {
        if (attached.pRoot != apRoot)
            continue;
        if (attached.pHand == pHand && NodeTools::GetParent(attached.pCopy) == pHand)
            return;
        // the skeleton was rebuilt or the copy taken off: put on anew
        NodeTools::Detach(attached.pCopy);
        NodeTools::DecRef(attached.pCopy);
        attached = m_attached.back();
        m_attached.pop_back();
        break;
    }
    if (!pHand)
        return;

    uint8_t* pCopy = NodeTools::Clone(m_pSource);
    if (!pCopy)
        return;
    NodeTools::IncRef(pCopy);
    NodeTools::Write(pCopy, NodeTools::kLocalOffset, OffHandModel::OnWeaponLeft());
    NodeTools::SetCulled(pCopy, !m_shown);
    NodeTools::AttachChild(pHand, pCopy);
    m_attached.push_back({apRoot, pHand, pCopy, nullptr});
}

void OffHandModel::SetShown(bool aShown) noexcept
{
    m_shown = aShown;
    for (auto& attached : m_attached)
        NodeTools::SetCulled(attached.pCopy, !aShown);
}

void OffHandModel::Flash(const std::string& acModel, float aSeconds) noexcept
{
    static BSFixedString s_muzzle("ProjectileNode");
    if (acModel.empty() || m_attached.empty())
        return;

    if (acModel != m_flashModel)
    {
        // another weapon's flash: the old copies go (the source is made again below)
        for (auto& attached : m_attached)
        {
            if (attached.pFlash)
                NodeTools::Detach(attached.pFlash);
            attached.pFlash = nullptr;
        }
        DropFlashSource();
        m_flashModel = acModel;

        // BSModelDB::Demand (CommonLibF4 id 1225688): the model as an object's, prepared after loading, processed, no
        // fade node, its textures loaded (ArgsType: LOD fade mult, load level, flag bits 0, 3 and 5), as RDVehicles
        // loads its bike
        struct ArgsType
        {
            int32_t LodFadeMult;
            uint32_t LoadLevel;
            uint8_t Flags;
            uint8_t Pad[3];
        };
        static_assert(sizeof(ArgsType) == 0x0C);
        using TDemand = uint32_t(const char*, uint8_t**, const ArgsType&);
        POINTER_FALLOUT4(TDemand, s_demand, 1225689);

        const ArgsType args{1, 0, 0x29, {}};
        uint8_t* pLoaded = nullptr;
        uint32_t error = s_demand.Get()(("Meshes\\" + acModel).c_str(), &pLoaded, args);
        if (error != 0 || !pLoaded)
        {
            NodeTools::DecRef(pLoaded);
            pLoaded = nullptr;
            error = s_demand.Get()(acModel.c_str(), &pLoaded, args);
        }
        if (error != 0 || !pLoaded)
        {
            NodeTools::DecRef(pLoaded);
            spdlog::warn("Dual wield: couldn't load the muzzle flash {} (error {})", acModel, error);
            return;
        }
        // our own copy to make the flashes from; the loaded one is the model database's
        m_pFlashSource = NodeTools::Clone(pLoaded);
        NodeTools::IncRef(m_pFlashSource);
        NodeTools::DecRef(pLoaded);
    }
    if (!m_pFlashSource)
        return;

    for (auto& attached : m_attached)
    {
        if (!attached.pFlash)
        {
            uint8_t* pMuzzle = NodeTools::GetObjectByName(attached.pCopy, s_muzzle);
            if (!pMuzzle)
                continue;
            attached.pFlash = NodeTools::Clone(m_pFlashSource);
            if (!attached.pFlash)
                continue;
            NodeTools::Write(attached.pFlash, NodeTools::kLocalOffset, NodeTools::Transform{});
            NodeTools::AttachChild(pMuzzle, attached.pFlash);
        }
        NodeTools::SetCulled(attached.pFlash, false);
    }
    m_flashUntil = NowMs() + static_cast<int64_t>(std::clamp(aSeconds, 0.02f, 0.5f) * 1000.f);
}

void OffHandModel::DropFlashSource() noexcept
{
    if (m_pFlashSource)
    {
        NodeTools::DecRef(m_pFlashSource);
        m_pFlashSource = nullptr;
    }
    m_flashModel.clear();
    m_flashUntil = 0;
}

void OffHandModel::DetachFrom(uint8_t* apRoot) noexcept
{
    for (size_t i = 0; i < m_attached.size(); ++i)
    {
        if (m_attached[i].pRoot != apRoot)
            continue;
        NodeTools::Detach(m_attached[i].pCopy);
        NodeTools::DecRef(m_attached[i].pCopy);
        m_attached[i] = m_attached.back();
        m_attached.pop_back();
        return;
    }
}

void OffHandModel::DetachAll() noexcept
{
    for (auto& attached : m_attached)
    {
        NodeTools::Detach(attached.pCopy);
        NodeTools::DecRef(attached.pCopy);
    }
    m_attached.clear();
}

uint8_t* OffHandModel::GetAttached(uint8_t* apRoot) const noexcept
{
    for (const auto& attached : m_attached)
    {
        if (attached.pRoot == apRoot)
            return attached.pCopy;
    }
    return nullptr;
}

void OffHandModel::Reset() noexcept
{
    DetachAll();
    DropBuildReference();
    DropFlashSource();
    if (m_pSource)
    {
        NodeTools::DecRef(m_pSource);
        m_pSource = nullptr;
    }
    m_weaponId = 0;
}
