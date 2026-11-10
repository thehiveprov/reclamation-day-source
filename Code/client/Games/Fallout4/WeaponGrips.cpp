#include <HivemindPCH.h>

#include <WeaponGrips.h>

#include <Components/BGSKeywordForm.h>
#include <Forms/TESForm.h>

#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <intrin.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
using NodeTools::Transform;

// A first person weapon subgraph: the weapon keywords it needs (all of them; Fallout4.esm ids) and the folders it takes
// clips from, in order
struct Subgraph
{
    std::array<uint32_t, 4> Keywords;
    uint8_t Count;
    std::array<const char*, 4> Folders;
};

constexpr Subgraph kSubgraphs[] = {
#include "WeaponGripsTable.inl"
};

constexpr char kAnimations[] = "Meshes\\Actors\\Character\\_1stPerson\\Animations\\";
constexpr char kSkeleton[] = "Meshes\\Actors\\Character\\_1stPerson\\CharacterAssets\\skeleton.hkx";
// the third person ones (Fallout4 - Animations.ba2; their clips spline compressed, bound to the skeleton's bones in order)
constexpr char kThirdPersonAnimations[] = "Meshes\\Actors\\Character\\Animations\\";
constexpr char kThirdPersonSkeleton[] = "Meshes\\Actors\\Character\\CharacterAssets\\skeleton.hkx";
// without a subgraph of their own: the 10mm's for a gun, the one-handed melee weapons' for a melee weapon
constexpr char kDefaultGun[] = "Pistol";
constexpr char kDefaultMelee[] = "1HM";
constexpr size_t kMaxFileSize = 16 * 1024 * 1024;
constexpr float kPi = 3.14159265f;

bool HasKeywordId(const BGSKeywordForm* apKeywords, uint32_t aId) noexcept
{
    for (uint32_t i = 0; apKeywords && apKeywords->keywords && i < apKeywords->count; ++i)
    {
        const auto* pKeyword = reinterpret_cast<const TESForm*>(apKeywords->keywords[i]);
        if (pKeyword && pKeyword->formID == aId)
            return true;
    }
    return false;
}

// The folders a weapon's clips come from, in the order the game looks: the subgraph the game would pick (all its keywords
// on the weapon, the most of them), then the default one
std::vector<std::string> FoldersFor(const BGSKeywordForm* apKeywords, bool aMelee) noexcept
{
    const Subgraph* pBest = nullptr;
    for (const Subgraph& subgraph : kSubgraphs)
    {
        if (pBest && subgraph.Count <= pBest->Count)
            continue;
        bool all = true;
        for (uint8_t i = 0; i < subgraph.Count && all; ++i)
            all = HasKeywordId(apKeywords, subgraph.Keywords[i]);
        if (all)
            pBest = &subgraph;
    }
    std::vector<std::string> folders;
    if (pBest)
    {
        for (const char* pFolder : pBest->Folders)
        {
            if (pFolder)
                folders.emplace_back(pFolder);
        }
    }
    folders.emplace_back(aMelee ? kDefaultMelee : kDefaultGun);
    return folders;
}

// A file of the game's (Data's loose files, then the archives, as the game finds them): BSResource::GetOrCreateStream
// (CommonLibF4 id 1143155), then the stream's DoOpen (vtable slot 1), its size (StreamBase::totalSize, 0x08), DoRead
// (slot 6), DoClose (slot 2); the smart pointer's reference let go as BSTSmartPointerIntrusiveRefCount does (the count in
// StreamBase::flags' high bits, 0x0C; the last one deletes it: the deleting destructor, slot 0)
std::vector<uint8_t> ReadGameFile(const std::string& acPath) noexcept
{
    using TGetOrCreateStream = uint32_t(const char*, uint8_t**, bool, void*);
    POINTER_FALLOUT4(TGetOrCreateStream, s_getOrCreateStream, 1143156);
    std::vector<uint8_t> bytes;
    uint8_t* pStream = nullptr;
    if (s_getOrCreateStream.Get()(acPath.c_str(), &pStream, false, nullptr) != 0 || !pStream)
        return bytes;

    void** pVtable = *reinterpret_cast<void***>(pStream);
    using TOpen = uint32_t(uint8_t*);
    using TClose = void(uint8_t*);
    using TRead = uint32_t(uint8_t*, void*, uint64_t, uint64_t*);
    if (reinterpret_cast<TOpen*>(pVtable[1])(pStream) == 0)
    {
        const uint32_t size = *reinterpret_cast<const uint32_t*>(pStream + 0x08);
        if (size > 0 && size <= kMaxFileSize)
        {
            bytes.resize(size);
            uint64_t read = 0;
            if (reinterpret_cast<TRead*>(pVtable[6])(pStream, bytes.data(), size, &read) != 0 || read != size)
                bytes.clear();
        }
        reinterpret_cast<TClose*>(pVtable[2])(pStream);
    }
    auto* pFlags = reinterpret_cast<volatile long*>(pStream + 0x0C);
    const auto left = static_cast<uint32_t>(_InterlockedExchangeAdd(pFlags, -0x1000) - 0x1000);
    if ((left & 0xFFFFF000u) == 0)
    {
        using TDelete = void(uint8_t*, uint32_t);
        reinterpret_cast<TDelete*>(pVtable[0])(pStream, 1);
    }
    return bytes;
}

// A Havok 2014 packfile (version 11, 64-bit): three section headers from 0x50 (0x40 each: the name, at +0x14 where the
// section starts, then from there its local fixups +0x18, global fixups +0x1C, virtual fixups +0x20, exports +0x24 and
// its end +0x2C); the data section's pointers are its local fixups (where a pointer is -> where it points), its objects
// its virtual fixups (where one is, the class name's place in the class names section)
class Packfile
{
public:
    explicit Packfile(std::vector<uint8_t> aBytes) noexcept
        : m_bytes(std::move(aBytes))
    {
        if (m_bytes.size() < 0x50 + 3 * 0x40 || U32(0) != 0x57E0E057 || U32(4) != 0x10C0C010 || U32(0x0C) != 11)
            return;
        uint32_t classNames = 0;
        uint32_t localFixups = 0;
        uint32_t globalFixups = 0;
        uint32_t virtualFixups = 0;
        uint32_t exports = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t header = 0x50 + i * 0x40;
            const std::string name(reinterpret_cast<const char*>(&m_bytes[header]), strnlen(reinterpret_cast<const char*>(&m_bytes[header]), 16));
            const uint32_t start = U32(header + 0x14);
            if (name == "__classnames__")
                classNames = start;
            else if (name == "__data__")
            {
                m_data = start;
                localFixups = start + U32(header + 0x18);
                globalFixups = start + U32(header + 0x1C);
                virtualFixups = start + U32(header + 0x20);
                exports = start + U32(header + 0x24);
            }
        }
        if (!m_data || !classNames)
            return;
        for (uint32_t at = localFixups; at + 8 <= globalFixups && at + 8 <= m_bytes.size(); at += 8)
        {
            if (U32(at) == 0xFFFFFFFF)
                break;
            m_pointers[U32(at)] = U32(at + 4);
        }
        for (uint32_t at = virtualFixups; at + 12 <= exports && at + 12 <= m_bytes.size(); at += 12)
        {
            if (U32(at) == 0xFFFFFFFF)
                break;
            const uint32_t name = classNames + U32(at + 8);
            if (name >= m_bytes.size())
                continue;
            m_objects.emplace_back(U32(at), std::string(reinterpret_cast<const char*>(&m_bytes[name]), strnlen(reinterpret_cast<const char*>(&m_bytes[name]), m_bytes.size() - name)));
        }
        m_valid = true;
    }

    [[nodiscard]] bool IsValid() const noexcept { return m_valid; }

    // the first object of that class (its place in the data section)
    [[nodiscard]] std::optional<uint32_t> Find(const char* acpClass) const noexcept
    {
        for (const auto& [at, name] : m_objects)
        {
            if (name == acpClass)
                return at;
        }
        return std::nullopt;
    }

    // An hkArray's elements (its place, how many) at a place in the data section; none when it has no pointer
    [[nodiscard]] std::pair<std::optional<uint32_t>, uint32_t> Array(uint32_t aAt) const noexcept
    {
        const auto it = m_pointers.find(aAt);
        return {it != m_pointers.end() ? std::optional<uint32_t>(it->second) : std::nullopt, DataU32(aAt + 8)};
    }

    // A pointed-to string (an hkStringPtr's)
    [[nodiscard]] std::string String(uint32_t aAt) const noexcept
    {
        const auto it = m_pointers.find(aAt);
        if (it == m_pointers.end() || size_t(m_data) + it->second >= m_bytes.size())
            return {};
        const char* pText = reinterpret_cast<const char*>(&m_bytes[m_data + it->second]);
        return std::string(pText, strnlen(pText, m_bytes.size() - (m_data + it->second)));
    }

    // values in the data section (0 past its end)
    [[nodiscard]] uint32_t DataU32(uint32_t aAt) const noexcept { return U32(size_t(m_data) + aAt); }
    [[nodiscard]] uint16_t DataU16(uint32_t aAt) const noexcept
    {
        const size_t at = size_t(m_data) + aAt;
        return at + 2 <= m_bytes.size() ? *reinterpret_cast<const uint16_t*>(&m_bytes[at]) : 0;
    }
    [[nodiscard]] float DataF32(uint32_t aAt) const noexcept
    {
        const uint32_t bits = DataU32(aAt);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    [[nodiscard]] bool InData(uint32_t aAt, size_t aBytes) const noexcept { return size_t(m_data) + aAt + aBytes <= m_bytes.size(); }
    // aBytes of the data section from a place (null past its end)
    [[nodiscard]] const uint8_t* DataBytes(uint32_t aAt, size_t aBytes) const noexcept { return InData(aAt, aBytes) ? &m_bytes[size_t(m_data) + aAt] : nullptr; }

private:
    [[nodiscard]] uint32_t U32(size_t aAt) const noexcept { return aAt + 4 <= m_bytes.size() ? *reinterpret_cast<const uint32_t*>(&m_bytes[aAt]) : 0; }

    std::vector<uint8_t> m_bytes;
    uint32_t m_data{0};
    std::unordered_map<uint32_t, uint32_t> m_pointers;
    std::vector<std::pair<uint32_t, std::string>> m_objects;
    bool m_valid{false};
};

// The first person skeleton, and the third person one (hkaSkeleton: its parents at +0x18, int16; its bones at +0x28, 0x10
// each, the name's pointer first; its rest pose at +0x38, hkQsTransform 0x30 each, the translation first), each read
// once: its bones' names, the bones the right arm's grip and its place in the view come from (-1 for none), and the index
// finger's rest offset on the hand (a clip that doesn't have it there was read wrong: a finger's length never changes)
struct Skeleton
{
    std::vector<int16_t> Parents;
    std::vector<std::string> Names;
    glm::vec3 FingerRest{0.f};
    int Weapon{-1};
    int Hand{-1};
    int Elbow{-1};
    int Camera{-1};
    std::array<int, WeaponGrips::kFingerCount> Fingers{};
    bool Valid{false};
};

Skeleton ReadSkeleton(const char* acpPath, bool aFirstPerson) noexcept
{
    Skeleton skeleton{};
    skeleton.Fingers.fill(-1);
    const Packfile file(ReadGameFile(acpPath));
    const auto at = file.IsValid() ? file.Find("hkaSkeleton") : std::nullopt;
    if (!at)
    {
        spdlog::warn("Dual wield: couldn't read {} (the left hand keeps the game's {})", acpPath, aFirstPerson ? "grip" : "arm in third person");
        return skeleton;
    }
    const auto [parents, parentCount] = file.Array(*at + 0x18);
    for (uint32_t i = 0; parents && i < parentCount; ++i)
        skeleton.Parents.push_back(static_cast<int16_t>(file.DataU16(*parents + i * 2)));
    const auto [names, count] = file.Array(*at + 0x28);
    for (uint32_t i = 0; names && i < count; ++i)
    {
        const std::string name = file.String(*names + i * 0x10);
        skeleton.Names.push_back(name);
        const int bone = static_cast<int>(i);
        if (name == "Weapon")
            skeleton.Weapon = bone;
        else if (name == "RArm_Hand")
            skeleton.Hand = bone;
        else if (name == "RArm_ForeArm1")
            skeleton.Elbow = bone;
        else if (name == "Camera")
            skeleton.Camera = bone;
        for (size_t finger = 0; finger < WeaponGrips::kFingerCount; ++finger)
        {
            if (name == fmt::format("RArm_Finger{}{}", finger / 3 + 1, finger % 3 + 1))
                skeleton.Fingers[finger] = bone;
        }
    }
    const auto [rest, restCount] = file.Array(*at + 0x38);
    if (rest && skeleton.Fingers[3] >= 0 && static_cast<uint32_t>(skeleton.Fingers[3]) < restCount)
    {
        const uint32_t finger = *rest + skeleton.Fingers[3] * 0x30;
        skeleton.FingerRest = glm::vec3(file.DataF32(finger), file.DataF32(finger + 4), file.DataF32(finger + 8));
    }
    // (the third person skeleton has no Camera bone)
    skeleton.Valid = skeleton.Parents.size() == count && glm::length(skeleton.FingerRest) > 1.f && skeleton.Weapon >= 0 && skeleton.Hand >= 0 && skeleton.Elbow >= 0 &&
                     (skeleton.Camera >= 0 || !aFirstPerson) && std::all_of(skeleton.Fingers.begin(), skeleton.Fingers.end(), [](int aBone) { return aBone >= 0; });
    if (!skeleton.Valid)
        spdlog::warn("Dual wield: {} lacks the right arm's bones (the left hand keeps the game's {})", acpPath, aFirstPerson ? "grip" : "arm in third person");
    return skeleton;
}

const Skeleton& FirstPersonSkeleton() noexcept
{
    static const Skeleton s_skeleton = ReadSkeleton(kSkeleton, true);
    return s_skeleton;
}

const Skeleton& ThirdPersonSkeleton() noexcept
{
    static const Skeleton s_skeleton = ReadSkeleton(kThirdPersonSkeleton, false);
    return s_skeleton;
}

Transform FromQuat(const glm::quat& acQ, const glm::vec3& acTranslation) noexcept
{
    Transform place{};
    // the rotation's columns are the bone's axes on its parent: NiTransform's rows (NodeTools)
    const glm::mat3 m = glm::mat3_cast(glm::normalize(acQ));
    for (int row = 0; row < 3; ++row)
        place.Rows[row] = m[row];
    place.Translation = acTranslation;
    return place;
}

glm::quat ToQuat(const Transform& acTransform) noexcept
{
    return glm::quat_cast(glm::mat3(acTransform.Rows[0], acTransform.Rows[1], acTransform.Rows[2]));
}

// A decoded clip: every track's local transform at every frame, the bone each track drives, its events
struct Decoded
{
    uint32_t Frames{0};
    uint32_t Tracks{0};
    float Duration{0.f};
    std::vector<Transform> Locals; // frame * Tracks + track
    std::vector<int> TrackOfBone;
    std::vector<std::pair<float, std::string>> Events;

    [[nodiscard]] std::optional<Transform> Local(int aBone, uint32_t aFrame) const noexcept
    {
        int track = -1;
        if (!TrackOfBone.empty())
            track = static_cast<size_t>(aBone) < TrackOfBone.size() ? TrackOfBone[aBone] : -1;
        else
            track = static_cast<uint32_t>(aBone) < Tracks ? aBone : -1;
        if (track < 0 || aFrame >= Frames)
            return std::nullopt;
        return Locals[size_t(aFrame) * Tracks + track];
    }
};

// hkaLosslessCompressedAnimation (worked out from the game's clips, 2026-10-24; Tools/DualWieldGrips/lossless.py):
// hkaAnimation's track count at +0x18, then hkArrays (pointer, size, capacity) of dynamic translations +0x38 (floats),
// static translations +0x48, translation codes +0x58 (4 uint16 a track: x, y, z), dynamic rotations +0x68 (quaternions
// x y z w), static rotations +0x78, rotation codes +0x88 (uint16 a track), the frame count +0xD8. A code's low 2 bits: 0
// none (0, identity), 1 static, 2 dynamic; the rest its index (a dynamic value's per frame: frame * values a frame +
// index).
bool DecodeLossless(const Packfile& acFile, uint32_t aAt, Decoded& aOut) noexcept
{
    const uint32_t tracks = acFile.DataU32(aAt + 0x18);
    const uint32_t frames = acFile.DataU32(aAt + 0xD8);
    const auto [dynamicT, dynamicTCount] = acFile.Array(aAt + 0x38);
    const auto [staticT, staticTCount] = acFile.Array(aAt + 0x48);
    const auto [codesT, codesTCount] = acFile.Array(aAt + 0x58);
    const auto [dynamicR, dynamicRCount] = acFile.Array(aAt + 0x68);
    const auto [staticR, staticRCount] = acFile.Array(aAt + 0x78);
    const auto [codesR, codesRCount] = acFile.Array(aAt + 0x88);
    if (!frames || !tracks || frames > 10000 || !codesT || !codesR || codesTCount < tracks || codesRCount < tracks)
        return false;
    const uint32_t perFrameT = dynamicTCount / frames;
    const uint32_t perFrameR = dynamicRCount / frames;

    aOut.Frames = frames;
    aOut.Tracks = tracks;
    aOut.Locals.resize(size_t(frames) * tracks);
    bool ok = true;
    // where a code's value is for a frame (none: 0 / identity)
    const auto place = [&](uint16_t aCode, uint32_t aFrame, const std::optional<uint32_t>& acStatic, uint32_t aStaticCount, const std::optional<uint32_t>& acDynamic,
                           uint32_t aDynamicCount, uint32_t aPerFrame, uint32_t aSize) -> std::optional<uint32_t>
    {
        const uint32_t index = aCode >> 2;
        switch (aCode & 3)
        {
        case 0: return std::nullopt;
        case 1:
            if (acStatic && index < aStaticCount)
                return *acStatic + index * aSize;
            break;
        case 2:
            if (acDynamic && aFrame * aPerFrame + index < aDynamicCount)
                return *acDynamic + (aFrame * aPerFrame + index) * aSize;
            break;
        default: break;
        }
        ok = false;
        return std::nullopt;
    };
    for (uint32_t track = 0; track < tracks && ok; ++track)
    {
        uint16_t codes[3];
        for (int axis = 0; axis < 3; ++axis)
            codes[axis] = acFile.DataU16(*codesT + track * 8 + axis * 2);
        const uint16_t rotation = acFile.DataU16(*codesR + track * 2);
        for (uint32_t frame = 0; frame < frames && ok; ++frame)
        {
            glm::vec3 t(0.f);
            for (int axis = 0; axis < 3; ++axis)
            {
                if (const auto where = place(codes[axis], frame, staticT, staticTCount, dynamicT, dynamicTCount, perFrameT, 4))
                    t[axis] = acFile.DataF32(*where);
            }
            glm::quat q(1.f, 0.f, 0.f, 0.f);
            if (const auto where = place(rotation, frame, staticR, staticRCount, dynamicR, dynamicRCount, perFrameR, 16); where && acFile.InData(*where, 16))
                q = glm::quat(acFile.DataF32(*where + 12), acFile.DataF32(*where), acFile.DataF32(*where + 4), acFile.DataF32(*where + 8));
            if (glm::length(q) < 1e-4f)
                ok = false;
            aOut.Locals[size_t(frame) * tracks + track] = FromQuat(q, t);
        }
    }
    return ok;
}

// A little reader over a byte blob; past the end it reads 0 and marks itself failed
struct ByteReader
{
    const uint8_t* pBytes{nullptr};
    size_t Size{0};
    size_t At{0};
    bool Ok{true};

    uint8_t U8() noexcept
    {
        if (At + 1 > Size)
        {
            Ok = false;
            return 0;
        }
        return pBytes[At++];
    }
    uint16_t U16() noexcept
    {
        const uint16_t low = U8();
        return static_cast<uint16_t>(low | (U8() << 8));
    }
    uint32_t U32() noexcept
    {
        const uint32_t low = U16();
        return low | (static_cast<uint32_t>(U16()) << 16);
    }
    float F32() noexcept
    {
        const uint32_t bits = U32();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void Align(size_t aAlignment) noexcept
    {
        if (aAlignment > 1 && At % aAlignment)
            At += aAlignment - At % aAlignment;
    }
};

// The B-spline's knot span for a value, and the curve there (PyNifly's anim_fo4 _find_knot_span / _eval_bspline)
int KnotSpan(int aDegree, float aValue, int aControlPoints, const std::vector<float>& acKnots) noexcept
{
    if (aControlPoints <= 0 || static_cast<size_t>(aControlPoints) >= acKnots.size())
        return 0;
    if (aValue >= acKnots[aControlPoints])
        return aControlPoints - 1;
    int low = aDegree;
    int high = aControlPoints;
    int mid = (low + high) / 2;
    for (int i = 0; i < 100; ++i)
    {
        if (static_cast<size_t>(mid + 1) >= acKnots.size())
            break;
        if (aValue < acKnots[mid])
            high = mid;
        else if (aValue >= acKnots[mid + 1])
            low = mid;
        else
            break;
        mid = (low + high) / 2;
    }
    return mid;
}

// The basis weights at t for the span (degree + 1 of them, for control points span, span - 1...)
std::array<float, 8> Basis(int aSpan, int aDegree, float aT, const std::vector<float>& acKnots) noexcept
{
    std::array<float, 8> n{};
    n[0] = 1.f;
    for (int i = 1; i <= aDegree; ++i)
    {
        for (int j = i - 1; j >= 0; --j)
        {
            const int hi = aSpan + i - j;
            const int lo = aSpan - j;
            const float denominator = hi >= 0 && lo >= 0 && static_cast<size_t>(hi) < acKnots.size() ? acKnots[hi] - acKnots[lo] : 0.f;
            const float a = denominator >= 1e-10f ? (aT - acKnots[lo]) / denominator : 0.f;
            const float tmp = n[j] * a;
            if (j + 1 < static_cast<int>(n.size()))
                n[j + 1] += n[j] - tmp;
            n[j] = tmp;
        }
    }
    return n;
}

// Quantized quaternions (PyNifly's anim_fo4: 32, 40, 48 bits, uncompressed), each as x y z w
glm::vec4 ReadQuat(ByteReader& aReader, uint8_t aFormat) noexcept
{
    glm::vec4 q(0.f, 0.f, 0.f, 1.f);
    const auto withW = [](const glm::vec3& acV, float aW, uint32_t aShift)
    {
        switch (aShift)
        {
        case 0: return glm::vec4(aW, acV.x, acV.y, acV.z);
        case 1: return glm::vec4(acV.x, aW, acV.y, acV.z);
        case 2: return glm::vec4(acV.x, acV.y, aW, acV.z);
        default: return glm::vec4(acV.x, acV.y, acV.z, aW);
        }
    };
    switch (aFormat)
    {
    case 0:
    {
        const uint32_t value = aReader.U32();
        constexpr uint32_t kRMask = (1u << 10) - 1;
        float r = static_cast<float>((value >> 18) & kRMask) / static_cast<float>(kRMask);
        r = 1.f - r * r;
        const float phiTheta = static_cast<float>(value & 0x3FFFF);
        float phi = std::floor(std::sqrt(phiTheta));
        float theta = 0.f;
        if (phi > 0.f)
        {
            theta = (kPi / 4.f) * (phiTheta - phi * phi) / phi;
            phi = (kPi / 2.f / 511.f) * phi;
        }
        const float magnitude = std::sqrt(std::max(0.f, 1.f - r * r));
        q = glm::vec4(std::sin(phi) * std::cos(theta) * magnitude, std::sin(phi) * std::sin(theta) * magnitude, std::cos(phi) * magnitude, r);
        for (int i = 0; i < 4; ++i)
        {
            if (value & (0x10000000u << i))
                q[i] = -q[i];
        }
        break;
    }
    case 1:
    {
        uint64_t raw = 0;
        for (int i = 0; i < 5; ++i)
            raw |= static_cast<uint64_t>(aReader.U8()) << (8 * i);
        constexpr float kFraction = 0.000345436f;
        const glm::vec3 v((static_cast<float>(raw & 0xFFF) - 2049.f) * kFraction, (static_cast<float>((raw >> 12) & 0xFFF) - 2049.f) * kFraction,
                          (static_cast<float>((raw >> 24) & 0xFFF) - 2049.f) * kFraction);
        float w = std::sqrt(std::max(0.f, 1.f - glm::dot(v, v)));
        if ((raw >> 38) & 1)
            w = -w;
        q = withW(v, w, static_cast<uint32_t>((raw >> 36) & 3));
        break;
    }
    case 2:
    {
        const uint16_t x = aReader.U16();
        const uint16_t y = aReader.U16();
        const uint16_t z = aReader.U16();
        constexpr uint32_t kMask = (1u << 15) - 1;
        constexpr float kHalf = static_cast<float>(kMask >> 1);
        constexpr float kFraction = 0.000043161f;
        const uint32_t shift = ((y >> 14) & 2) | ((x >> 15) & 1);
        const glm::vec3 v((static_cast<float>(x & kMask) - kHalf) * kFraction, (static_cast<float>(y & kMask) - kHalf) * kFraction, (static_cast<float>(z & kMask) - kHalf) * kFraction);
        float w = std::sqrt(std::max(0.f, 1.f - glm::dot(v, v)));
        if ((z >> 15) != 0)
            w = -w;
        q = withW(v, w, shift);
        break;
    }
    case 5:
    {
        q.x = aReader.F32();
        q.y = aReader.F32();
        q.z = aReader.F32();
        q.w = aReader.F32();
        break;
    }
    default: aReader.Ok = false; break;
    }
    const float length = glm::length(q);
    return length > 1e-10f ? q / length : glm::vec4(0.f, 0.f, 0.f, 1.f);
}

size_t QuatAlignment(uint8_t aFormat) noexcept
{
    switch (aFormat)
    {
    case 0:
    case 5: return 4;
    case 2:
    case 4: return 2;
    default: return 1;
    }
}

// A translation or scale's three axes in a block (each none, static or a spline), every frame of the block
bool ReadVectorTrack(ByteReader& aReader, uint8_t aFlags, uint8_t aQuantization, float aIdentity, uint32_t aBlockFrames, std::vector<glm::vec3>& aOut) noexcept
{
    const auto typeOf = [aFlags](int aAxis) { return ((aFlags >> (aAxis + 4)) & 1) ? 2 : ((aFlags >> aAxis) & 1) ? 1 : 0; };
    aOut.assign(aBlockFrames, glm::vec3(aIdentity));
    const bool anySpline = typeOf(0) == 2 || typeOf(1) == 2 || typeOf(2) == 2;
    if (anySpline)
    {
        const uint16_t items = aReader.U16();
        const int degree = aReader.U8();
        if (degree > 6)
            return false;
        std::vector<float> knots(size_t(items) + degree + 2);
        for (float& knot : knots)
            knot = aReader.U8();
        aReader.Align(4);
        float minimum[3]{};
        float maximum[3]{};
        float fixed[3]{aIdentity, aIdentity, aIdentity};
        for (int axis = 0; axis < 3; ++axis)
        {
            if (typeOf(axis) == 2)
            {
                minimum[axis] = aReader.F32();
                maximum[axis] = aReader.F32();
            }
            else if (typeOf(axis) == 1)
                fixed[axis] = aReader.F32();
        }
        std::vector<float> points[3];
        for (uint32_t item = 0; item <= items; ++item)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (typeOf(axis) != 2)
                    continue;
                const float unit = aQuantization == 0 ? aReader.U8() / 255.f : aReader.U16() / 65535.f;
                points[axis].push_back(minimum[axis] + (maximum[axis] - minimum[axis]) * unit);
            }
        }
        aReader.Align(4);
        for (uint32_t frame = 0; frame < aBlockFrames; ++frame)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (typeOf(axis) != 2)
                {
                    aOut[frame][axis] = fixed[axis];
                    continue;
                }
                const int count = static_cast<int>(points[axis].size());
                const int span = KnotSpan(degree, static_cast<float>(frame), count, knots);
                const auto basis = Basis(span, degree, static_cast<float>(frame), knots);
                float value = 0.f;
                for (int i = 0; i <= degree; ++i)
                {
                    const int index = span - i;
                    if (index >= 0 && index < count)
                        value += points[axis][index] * basis[i];
                }
                aOut[frame][axis] = value;
            }
        }
    }
    else
    {
        glm::vec3 fixed(aIdentity);
        for (int axis = 0; axis < 3; ++axis)
        {
            if (typeOf(axis) == 1)
                fixed[axis] = aReader.F32();
        }
        aOut.assign(aBlockFrames, fixed);
    }
    aReader.Align(4);
    return aReader.Ok;
}

// hkaSplineCompressedAnimation (PyNifly's anim_fo4 _decompress_spline, its layout): frames +0x38, blocks +0x3C, frames a
// block +0x40, masks and quantization's size +0x44, block offsets +0x58 (uint32 array), the data +0x98 (bytes). Each
// block: a 4 byte mask a track (position, rotation, scale quantization; their flags), then each track's position,
// rotation and scale, static or a B-spline over the block's frames.
bool DecodeSpline(const Packfile& acFile, uint32_t aAt, Decoded& aOut) noexcept
{
    const uint32_t tracks = acFile.DataU32(aAt + 0x18);
    const uint32_t frames = acFile.DataU32(aAt + 0x38);
    const uint32_t blocks = acFile.DataU32(aAt + 0x3C);
    const uint32_t blockFrames = acFile.DataU32(aAt + 0x40);
    uint32_t maskSize = acFile.DataU32(aAt + 0x44);
    const auto [offsets, offsetCount] = acFile.Array(aAt + 0x58);
    const auto [data, dataSize] = acFile.Array(aAt + 0x98);
    if (!tracks || !frames || frames > 10000 || !blocks || !blockFrames || !offsets || offsetCount < blocks || !data)
        return false;
    const uint8_t* pData = acFile.DataBytes(*data, dataSize);
    if (!pData)
        return false;
    if (!maskSize)
        maskSize = (4 * tracks + 3) & ~3u;

    aOut.Frames = frames;
    aOut.Tracks = tracks;
    aOut.Locals.resize(size_t(frames) * tracks);
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> scales;
    for (uint32_t block = 0; block < blocks; ++block)
    {
        const uint32_t first = block * blockFrames;
        if (first >= frames)
            break;
        const uint32_t count = block == blocks - 1 ? frames - first : std::min(blockFrames, frames - first);
        ByteReader reader{pData, dataSize, acFile.DataU32(*offsets + block * 4)};
        const size_t masks = reader.At;
        reader.At = masks + maskSize;
        for (uint32_t track = 0; track < tracks; ++track)
        {
            if (masks + track * 4 + 4 > dataSize)
                return false;
            const uint8_t quantization = pData[masks + track * 4];
            const uint8_t positionFlags = pData[masks + track * 4 + 1];
            const uint8_t rotationFlags = pData[masks + track * 4 + 2];
            const uint8_t scaleFlags = pData[masks + track * 4 + 3];
            const uint8_t positionQuantization = quantization & 3;
            const uint8_t rotationQuantization = (quantization >> 2) & 0xF;
            const uint8_t scaleQuantization = (quantization >> 6) & 3;

            if (!ReadVectorTrack(reader, positionFlags, positionQuantization, 0.f, count, positions))
                return false;

            std::vector<glm::vec4> rotations(count, glm::vec4(0.f, 0.f, 0.f, 1.f));
            const size_t alignment = QuatAlignment(rotationQuantization);
            if ((rotationFlags >> 4) & 0xF)
            {
                const uint16_t items = reader.U16();
                const int degree = reader.U8();
                if (degree > 6)
                    return false;
                std::vector<float> knots(size_t(items) + degree + 2);
                for (float& knot : knots)
                    knot = reader.U8();
                reader.Align(alignment);
                std::vector<glm::vec4> points;
                for (uint32_t item = 0; item <= items; ++item)
                {
                    glm::vec4 q = ReadQuat(reader, rotationQuantization);
                    if (!points.empty() && glm::dot(q, points.back()) < 0.f)
                        q = -q;
                    points.push_back(q);
                }
                for (uint32_t frame = 0; frame < count; ++frame)
                {
                    const int pointCount = static_cast<int>(points.size());
                    const int span = KnotSpan(degree, static_cast<float>(frame), pointCount, knots);
                    const auto basis = Basis(span, degree, static_cast<float>(frame), knots);
                    glm::vec4 q(0.f);
                    for (int i = 0; i <= degree; ++i)
                    {
                        const int index = span - i;
                        if (index >= 0 && index < pointCount)
                            q += points[index] * basis[i];
                    }
                    const float length = glm::length(q);
                    rotations[frame] = length > 1e-10f ? q / length : glm::vec4(0.f, 0.f, 0.f, 1.f);
                }
            }
            else if (rotationFlags & 0xF)
            {
                reader.Align(alignment);
                rotations.assign(count, ReadQuat(reader, rotationQuantization));
            }
            reader.Align(4);

            if (!ReadVectorTrack(reader, scaleFlags, scaleQuantization, 1.f, count, scales) || !reader.Ok)
                return false;

            for (uint32_t frame = 0; frame < count; ++frame)
            {
                const glm::vec4& q = rotations[frame];
                aOut.Locals[size_t(first + frame) * tracks + track] = FromQuat(glm::quat(q.w, q.x, q.y, q.z), positions[frame]);
            }
        }
    }
    return true;
}

// A clip of the first person skeleton's: its animation (either kind), its tracks' bones (hkaAnimationBinding's
// transformTrackToBoneIndices, +0x20, int16, when it has them, else the skeleton's order), its events (hkaAnimation's
// annotation tracks, +0x28, 0x18 each: their annotations at +0x08, 0x10 each: the time, then the text's pointer)
std::optional<Decoded> Decode(const std::string& acPath, const Skeleton& acSkeleton) noexcept
{
    const Packfile file(ReadGameFile(acPath));
    if (!file.IsValid())
        return std::nullopt;
    Decoded decoded{};
    uint32_t animation = 0;
    if (const auto lossless = file.Find("hkaLosslessCompressedAnimation"))
    {
        animation = *lossless;
        if (!DecodeLossless(file, animation, decoded))
        {
            spdlog::warn("Dual wield: {} couldn't be read (its lossless animation)", acPath);
            return std::nullopt;
        }
    }
    else if (const auto spline = file.Find("hkaSplineCompressedAnimation"))
    {
        animation = *spline;
        if (!DecodeSpline(file, animation, decoded))
        {
            spdlog::warn("Dual wield: {} couldn't be read (its spline animation)", acPath);
            return std::nullopt;
        }
    }
    else
    {
        spdlog::warn("Dual wield: {} has no animation this reads", acPath);
        return std::nullopt;
    }
    decoded.Duration = file.DataF32(animation + 0x14);

    if (const auto binding = file.Find("hkaAnimationBinding"))
    {
        const auto [indices, count] = file.Array(*binding + 0x20);
        for (uint32_t track = 0; indices && track < count; ++track)
        {
            const auto bone = static_cast<int16_t>(file.DataU16(*indices + track * 2));
            if (bone < 0)
                continue;
            if (decoded.TrackOfBone.size() <= static_cast<size_t>(bone))
                decoded.TrackOfBone.resize(bone + 1, -1);
            decoded.TrackOfBone[bone] = static_cast<int>(track);
        }
    }

    const auto [annotationTracks, trackCount] = file.Array(animation + 0x28);
    for (uint32_t track = 0; annotationTracks && track < trackCount; ++track)
    {
        const auto [annotations, count] = file.Array(*annotationTracks + track * 0x18 + 0x08);
        for (uint32_t i = 0; annotations && i < count; ++i)
        {
            const uint32_t at = *annotations + i * 0x10;
            decoded.Events.emplace_back(file.DataF32(at), file.String(at + 8));
        }
    }
    std::sort(decoded.Events.begin(), decoded.Events.end(), [](const auto& acA, const auto& acB) { return acA.first < acB.first; });

    // read right: the index finger where the skeleton has it on the hand (its track through the binding)
    const auto finger = decoded.Local(acSkeleton.Fingers[3], 0);
    if (!finger || glm::distance(finger->Translation, acSkeleton.FingerRest) > 0.05f)
    {
        spdlog::warn("Dual wield: {} read wrong (its index finger at {:.2f} {:.2f} {:.2f}, the skeleton's at {:.2f} {:.2f} {:.2f}), not used", acPath,
                     finger ? finger->Translation.x : 0.f, finger ? finger->Translation.y : 0.f, finger ? finger->Translation.z : 0.f, acSkeleton.FingerRest.x,
                     acSkeleton.FingerRest.y, acSkeleton.FingerRest.z);
        return std::nullopt;
    }
    return decoded;
}

std::optional<WeaponGrips::Grip> GripAt(const Decoded& acClip, const Skeleton& acSkeleton, uint32_t aFrame) noexcept
{
    WeaponGrips::Grip grip{};
    const auto weapon = acClip.Local(acSkeleton.Weapon, aFrame);
    if (!weapon)
        return std::nullopt;
    grip.Weapon = *weapon;
    for (size_t finger = 0; finger < WeaponGrips::kFingerCount; ++finger)
    {
        const auto place = acClip.Local(acSkeleton.Fingers[finger], aFrame);
        if (!place)
            return std::nullopt;
        grip.Fingers[finger] = *place;
    }
    return grip;
}

// A bone's place in the clip's skeleton at a frame (its parents' places first; missing tracks: none)
std::optional<Transform> WorldAt(const Decoded& acClip, const Skeleton& acSkeleton, int aBone, uint32_t aFrame) noexcept
{
    std::vector<int> chain;
    for (int bone = aBone; bone >= 0 && chain.size() < 64; bone = static_cast<size_t>(bone) < acSkeleton.Parents.size() ? acSkeleton.Parents[bone] : -1)
        chain.push_back(bone);
    Transform world{};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        const auto local = acClip.Local(*it, aFrame);
        if (!local)
            return std::nullopt;
        world = it == chain.rbegin() ? *local : NodeTools::Combine(world, *local);
    }
    return world;
}

std::unordered_map<std::string, std::unique_ptr<WeaponGrips::Grip>> s_grips;
std::unordered_map<std::string, std::unique_ptr<WeaponGrips::ArmClip>> s_clips;

const WeaponGrips::Grip* ReadGrip(const std::string& acPath) noexcept
{
    if (const auto it = s_grips.find(acPath); it != s_grips.end())
        return it->second.get();
    std::unique_ptr<WeaponGrips::Grip> pGrip;
    const Skeleton& skeleton = FirstPersonSkeleton();
    if (skeleton.Valid)
    {
        if (const auto clip = Decode(acPath, skeleton))
        {
            if (const auto grip = GripAt(*clip, skeleton, 0))
            {
                pGrip = std::make_unique<WeaponGrips::Grip>(*grip);
                spdlog::info("Dual wield: the left hand's grip read from {}", acPath);
            }
        }
    }
    return s_grips.emplace(acPath, std::move(pGrip)).first->second.get();
}

const WeaponGrips::ArmClip* ReadClip(const std::string& acPath) noexcept
{
    if (const auto it = s_clips.find(acPath); it != s_clips.end())
        return it->second.get();
    std::unique_ptr<WeaponGrips::ArmClip> pClip;
    const Skeleton& skeleton = FirstPersonSkeleton();
    if (skeleton.Valid)
    {
        if (const auto clip = Decode(acPath, skeleton))
        {
            auto arm = std::make_unique<WeaponGrips::ArmClip>();
            arm->Duration = clip->Duration;
            arm->FrameTime = clip->Frames > 1 ? clip->Duration / static_cast<float>(clip->Frames - 1) : 1.f / 30.f;
            arm->Events = clip->Events;
            bool ok = true;
            for (uint32_t frame = 0; frame < clip->Frames && ok; ++frame)
            {
                const auto camera = WorldAt(*clip, skeleton, skeleton.Camera, frame);
                const auto hand = WorldAt(*clip, skeleton, skeleton.Hand, frame);
                const auto elbow = WorldAt(*clip, skeleton, skeleton.Elbow, frame);
                const auto grip = GripAt(*clip, skeleton, frame);
                if (!camera || !hand || !elbow || !grip)
                {
                    ok = false;
                    break;
                }
                WeaponGrips::ArmFrame armFrame{};
                armFrame.Hand = NodeTools::Relative(*camera, *hand);
                armFrame.Elbow = NodeTools::Relative(*camera, *elbow).Translation;
                armFrame.HandGrip = *grip;
                arm->Frames.push_back(armFrame);
            }
            if (ok && !arm->Frames.empty())
            {
                spdlog::info("Dual wield: the left arm's clip read from {} ({} frames, {:.2f} s, {} events)", acPath, arm->Frames.size(), arm->Duration, arm->Events.size());
                pClip = std::move(arm);
            }
        }
    }
    return s_clips.emplace(acPath, std::move(pClip)).first->second.get();
}

std::unordered_map<std::string, std::unique_ptr<WeaponGrips::LimbClip>> s_limbClips;

// A third person clip's right arm: every bone from RArm_Collarbone down that it moves (a Havok skeleton lists parents
// before their children), each frame's place on its parent
const WeaponGrips::LimbClip* ReadLimbClip(const std::string& acPath) noexcept
{
    if (const auto it = s_limbClips.find(acPath); it != s_limbClips.end())
        return it->second.get();
    std::unique_ptr<WeaponGrips::LimbClip> pLimb;
    const Skeleton& skeleton = ThirdPersonSkeleton();
    const auto collarbone = std::find(skeleton.Names.begin(), skeleton.Names.end(), "RArm_Collarbone");
    if (skeleton.Valid && collarbone != skeleton.Names.end())
    {
        if (const auto clip = Decode(acPath, skeleton))
        {
            auto limb = std::make_unique<WeaponGrips::LimbClip>();
            const int top = static_cast<int>(collarbone - skeleton.Names.begin());
            std::vector<int> bones;
            std::vector<int> inLimb(skeleton.Names.size(), -1);
            for (int bone = top; bone < static_cast<int>(skeleton.Names.size()); ++bone)
            {
                const int parent = skeleton.Parents[bone];
                if ((bone != top && (parent < 0 || inLimb[parent] < 0)) || !clip->Local(bone, 0))
                    continue;
                inLimb[bone] = static_cast<int>(limb->Bones.size());
                limb->Bones.push_back(skeleton.Names[bone]);
                limb->Parents.push_back(bone == top ? -1 : inLimb[parent]);
                bones.push_back(bone);
            }
            // and the body the arm swings from (the spine to the head: an attack turns it)
            for (const char* pBody : {"Spine1", "Spine2", "Chest", "Neck", "Head"})
            {
                const auto it = std::find(skeleton.Names.begin(), skeleton.Names.end(), pBody);
                const int bone = static_cast<int>(it - skeleton.Names.begin());
                if (it == skeleton.Names.end() || !clip->Local(bone, 0))
                    continue;
                limb->Bones.push_back(*it);
                limb->Parents.push_back(-1);
                bones.push_back(bone);
            }
            limb->Duration = clip->Duration;
            limb->FrameTime = clip->Frames > 1 ? clip->Duration / static_cast<float>(clip->Frames - 1) : 1.f / 30.f;
            limb->Events = clip->Events;
            limb->Locals.reserve(size_t(clip->Frames) * bones.size());
            for (uint32_t frame = 0; frame < clip->Frames; ++frame)
            {
                for (const int bone : bones)
                    limb->Locals.push_back(*clip->Local(bone, frame));
            }
            if (!bones.empty() && clip->Frames > 0)
            {
                spdlog::info("Dual wield: a third person clip read from {} ({} bones, {} frames, {:.2f} s, {} events)", acPath, bones.size(), clip->Frames, limb->Duration,
                             limb->Events.size());
                pLimb = std::move(limb);
            }
        }
    }
    return s_limbClips.emplace(acPath, std::move(pLimb)).first->second.get();
}

Transform BlendPlace(const Transform& acFrom, const Transform& acTo, float aT) noexcept
{
    Transform blended = FromQuat(glm::slerp(ToQuat(acFrom), ToQuat(acTo), aT), glm::mix(acFrom.Translation, acTo.Translation, aT));
    blended.Scale = acFrom.Scale;
    return blended;
}
} // namespace

namespace WeaponGrips
{
Grip Blend(const Grip& acFrom, const Grip& acTo, float aT) noexcept
{
    Grip blended{};
    blended.Weapon = BlendPlace(acFrom.Weapon, acTo.Weapon, aT);
    for (size_t i = 0; i < kFingerCount; ++i)
        blended.Fingers[i] = BlendPlace(acFrom.Fingers[i], acTo.Fingers[i], aT);
    return blended;
}

ArmFrame Blend(const ArmFrame& acFrom, const ArmFrame& acTo, float aT) noexcept
{
    ArmFrame blended{};
    blended.Hand = BlendPlace(acFrom.Hand, acTo.Hand, aT);
    blended.Elbow = glm::mix(acFrom.Elbow, acTo.Elbow, aT);
    blended.HandGrip = Blend(acFrom.HandGrip, acTo.HandGrip, aT);
    return blended;
}

ArmFrame ArmClip::At(float aTime) const noexcept
{
    if (Frames.empty())
        return {};
    const float frame = std::clamp(aTime / std::max(FrameTime, 1e-4f), 0.f, static_cast<float>(Frames.size() - 1));
    const size_t index = static_cast<size_t>(frame);
    const size_t next = std::min(index + 1, Frames.size() - 1);
    return Blend(Frames[index], Frames[next], frame - static_cast<float>(index));
}

Transform LimbClip::LocalAt(size_t aBone, float aTime) const noexcept
{
    const size_t count = Bones.size();
    if (count == 0 || aBone >= count || Locals.size() < count)
        return {};
    const size_t frames = Locals.size() / count;
    const float frame = std::clamp(aTime / std::max(FrameTime, 1e-4f), 0.f, static_cast<float>(frames - 1));
    const size_t index = static_cast<size_t>(frame);
    const size_t next = std::min(index + 1, frames - 1);
    return BlendPlace(Locals[index * count + aBone], Locals[next * count + aBone], frame - static_cast<float>(index));
}

float LimbClip::EventTime(const char* acpEvent, float aFallback) const noexcept
{
    for (const auto& [time, name] : Events)
    {
        if (_stricmp(name.c_str(), acpEvent) == 0)
            return time;
    }
    return aFallback;
}

float ArmClip::EventTime(const char* acpEvent, float aFallback) const noexcept
{
    for (const auto& [time, name] : Events)
    {
        if (_stricmp(name.c_str(), acpEvent) == 0)
            return time;
    }
    return aFallback;
}

const Grip* ForKeywords(const BGSKeywordForm* apKeywords, bool aMelee) noexcept
{
    const char* pClip = aMelee ? "Idle.hkx" : "WPNIdleReady.hkx";
    for (const std::string& folder : FoldersFor(apKeywords, aMelee))
    {
        if (const Grip* pGrip = ReadGrip(fmt::format("{}{}\\{}", kAnimations, folder, pClip)))
            return pGrip;
    }
    return nullptr;
}

const ArmClip* ClipFor(const BGSKeywordForm* apKeywords, bool aMelee, const char* acpClip) noexcept
{
    for (const std::string& folder : FoldersFor(apKeywords, aMelee))
    {
        if (const ArmClip* pClip = ReadClip(fmt::format("{}{}\\{}", kAnimations, folder, acpClip)))
            return pClip;
    }
    return nullptr;
}

const ArmClip* ClipIn(const char* acpFolder, const char* acpClip) noexcept
{
    return ReadClip(fmt::format("{}{}\\{}", kAnimations, acpFolder, acpClip));
}

const LimbClip* ThirdPersonClipFor(const BGSKeywordForm* apKeywords, bool aMelee, const char* acpClip) noexcept
{
    for (const std::string& folder : FoldersFor(apKeywords, aMelee))
    {
        if (const LimbClip* pClip = ReadLimbClip(fmt::format("{}{}\\{}", kThirdPersonAnimations, folder, acpClip)))
            return pClip;
    }
    return nullptr;
}

const LimbClip* ThirdPersonClipIn(const char* acpFolder, const char* acpClip) noexcept
{
    return ReadLimbClip(fmt::format("{}{}\\{}", kThirdPersonAnimations, acpFolder, acpClip));
}

const Grip* ThirdPersonGripFor(const BGSKeywordForm* apKeywords, bool aMelee) noexcept
{
    static std::unordered_map<const LimbClip*, std::unique_ptr<Grip>> s_thirdPersonGrips;
    const LimbClip* pIdle = ThirdPersonClipFor(apKeywords, aMelee, aMelee ? "Idle.hkx" : "WPNIdleReady.hkx");
    if (!pIdle)
        return nullptr;
    if (const auto it = s_thirdPersonGrips.find(pIdle); it != s_thirdPersonGrips.end())
        return it->second.get();
    // its first frame's weapon bone and fingers, each on its parent (the clip's names: Weapon, RArm_Finger11...)
    const auto find = [pIdle](const std::string& acName)
    {
        const auto it = std::find(pIdle->Bones.begin(), pIdle->Bones.end(), acName);
        return it != pIdle->Bones.end() ? static_cast<int>(it - pIdle->Bones.begin()) : -1;
    };
    std::unique_ptr<Grip> pGrip;
    const int weapon = find("Weapon");
    std::array<int, kFingerCount> fingers{};
    for (size_t i = 0; i < kFingerCount; ++i)
        fingers[i] = find(fmt::format("RArm_Finger{}{}", i / 3 + 1, i % 3 + 1));
    if (weapon >= 0 && std::all_of(fingers.begin(), fingers.end(), [](int aBone) { return aBone >= 0; }))
    {
        pGrip = std::make_unique<Grip>();
        pGrip->Weapon = pIdle->LocalAt(static_cast<size_t>(weapon), 0.f);
        for (size_t i = 0; i < kFingerCount; ++i)
            pGrip->Fingers[i] = pIdle->LocalAt(static_cast<size_t>(fingers[i]), 0.f);
        spdlog::info("Dual wield: the left hand's third person grip read from its idle");
    }
    return s_thirdPersonGrips.emplace(pIdle, std::move(pGrip)).first->second.get();
}
} // namespace WeaponGrips
