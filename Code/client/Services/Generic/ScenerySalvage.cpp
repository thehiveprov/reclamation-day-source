#if TP_FALLOUT4

#include <Services/ScenerySalvage.h>

#include <Forms/TESForm.h>
#include <TESObjectREFR.h>

#include <cctype>

namespace
{
// Fallout4.esm: the scrap items workbenches keep (c_..._scrap)
constexpr uint32_t kSteel = 0x731A4;
constexpr uint32_t kWood = 0x731A3;
constexpr uint32_t kConcrete = 0x106D99;
constexpr uint32_t kGlass = 0x69085;
constexpr uint32_t kPlastic = 0x6907F;
constexpr uint32_t kFertilizer = 0x1BF730;
constexpr uint32_t kCopper = 0x6907C;
constexpr uint32_t kCeramic = 0xAEC5E;
constexpr uint32_t kCloth = 0xAEC5F;
constexpr uint32_t kRubber = 0x106D98;
constexpr uint32_t kAluminum = 0x6907A;
constexpr uint32_t kScrews = 0x69081;
constexpr uint32_t kLeather = 0xAEC64;

// Form types whose mesh is their TESModel at 0x68 (STAT, SCOL, MSTT, TREE)
constexpr std::array<uint8_t, 4> kModelAt68Types{0x24, 0x25, 0x26, 0x28};

struct Yield
{
    uint32_t Item;
    float PerSize; // how many per unit of size (see GetSize)
};

// A kind of object: what in its mesh's path says so, what scrapping it gives. Looked at in this order.
struct Kind
{
    std::vector<const char*> Words;
    std::vector<Yield> Yields;
};

const std::vector<Kind>& GetKinds() noexcept
{
    static const std::vector<Kind> s_kinds{
        {{"tire"}, {{kRubber, 1.8f}, {kSteel, 0.2f}}},
        {{"glass", "window"}, {{kGlass, 1.3f}, {kWood, 0.3f}}},
        {{"vehicle", "car0", "carwreck", "truck", "bus0", "motorcycle", "wreck", "boat"},
         {{kSteel, 1.8f}, {kRubber, 0.5f}, {kGlass, 0.4f}, {kAluminum, 0.3f}, {kPlastic, 0.2f}}},
        {{"vine", "ivy"}, {{kFertilizer, 0.8f}, {kCloth, 0.4f}}},
        {{"bush", "shrub", "hedge"}, {{kWood, 0.8f}, {kFertilizer, 0.6f}}},
        {{"plants\\", "\\plant", "grass", "weed", "fern", "flower", "moss", "kelp", "cattail", "lily", "mushroom", "leaf", "leaves"},
         {{kFertilizer, 1.f}, {kWood, 0.2f}}},
        // Not "tree" alone: streets
        {{"trees\\", "\\tree", "trunk", "stump", "log0", "logs", "branch", "driftwood", "twig"}, {{kWood, 1.2f}, {kFertilizer, 0.3f}}},
        {{"rock", "boulder", "cliff", "stone", "gravel"}, {{kConcrete, 1.2f}, {kFertilizer, 0.2f}}},
        {{"trash", "garbage", "litter", "paper", "cardboard", "junk"}, {{kCloth, 0.5f}, {kPlastic, 0.5f}, {kWood, 0.3f}, {kSteel, 0.3f}}},
        {{"rubble", "debris", "brick", "cinder", "concrete", "sidewalk", "curb", "foundation", "pillar", "column", "masonry", "plaster", "asphalt"},
         {{kConcrete, 1.2f}, {kSteel, 0.4f}}},
        {{"ceramic", "tile", "toilet", "sink", "porcelain"}, {{kCeramic, 1.f}, {kSteel, 0.2f}}},
        {{"wire", "cable", "powerline"}, {{kCopper, 1.f}, {kRubber, 0.3f}}},
        {{"metal", "steel", "iron", "pipe", "girder", "beam", "rebar", "corrugated", "chainlink", "sign", "pole", "barrel", "tank", "vent", "duct", "rail",
          "scaffold", "tower", "machine", "generator", "container", "dumpster", "locker", "shelf", "cabinet"},
         {{kSteel, 1.3f}, {kScrews, 0.15f}}},
        {{"wood", "plank", "board", "crate", "pallet", "shack", "barn", "cabin", "picket", "porch", "deck", "shingle", "lumber", "fence"},
         {{kWood, 1.3f}, {kSteel, 0.1f}}},
        {{"cloth", "rug", "carpet", "curtain", "mattress", "tarp", "flag", "fabric"}, {{kCloth, 1.3f}, {kLeather, 0.1f}}},
        {{"plastic"}, {{kPlastic, 1.3f}}},
        {{"architecture\\", "buildings\\", "interiors\\"}, {{kConcrete, 1.f}, {kWood, 0.6f}, {kSteel, 0.4f}}},
        {{"landscape\\"}, {{kConcrete, 0.8f}, {kFertilizer, 0.3f}}},
    };    return s_kinds;
}

// What everything else gives
const std::vector<Yield> kOtherYields{{kSteel, 0.6f}, {kWood, 0.6f}};

// Words of mesh file names that say nothing about the object
const std::vector<std::string> kNoiseWords{"nf", "lod", "static", "var", "mod", "fx", "nocol", "nocollision", "anim", "dirty", "clean", "lo", "hi"};

std::string ToLower(std::string aText) noexcept
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](char aChar) { return static_cast<char>(std::tolower(static_cast<unsigned char>(aChar))); });
    return aText;
}

// The mesh of a base object that has its TESModel at 0x68 (TESModel::GetModel is its vfunc 7), empty for others
std::string GetModel(const TESForm* apBase) noexcept
{
    if (!apBase || std::find(kModelAt68Types.begin(), kModelAt68Types.end(), static_cast<uint8_t>(apBase->formType)) == kModelAt68Types.end())
        return {};

    const auto* pModel = reinterpret_cast<const uint8_t*>(apBase) + 0x68;
    using TGetModel = const char*(const void*);
    void* const* pVtable = *reinterpret_cast<void* const* const*>(pModel);
    const char* pPath = reinterpret_cast<TGetModel*>(pVtable[7])(pModel);
    return pPath ? pPath : "";
}

// "Landscape\Plants\VineHanging01.nif" -> "Vine Hanging": the file name's words, without numbers, letters alone or noise
std::string MakeLabel(const std::string& acModel) noexcept
{
    std::string name = acModel.substr(acModel.find_last_of("\\/") + 1);
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos)
        name.resize(dot);

    std::vector<std::string> words;
    std::string word;
    auto finish = [&]()
    {
        const std::string lower = ToLower(word);
        const bool number = !word.empty() && std::all_of(word.begin(), word.end(), [](char aChar) { return std::isdigit(static_cast<unsigned char>(aChar)); });
        if (word.size() > 2 && !number && std::find(kNoiseWords.begin(), kNoiseWords.end(), lower) == kNoiseWords.end())
        {
            std::string capitalized = lower;
            capitalized[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(capitalized[0])));
            words.push_back(capitalized);
        }
        word.clear();
    };

    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        if (c == '_' || c == '-' || c == ' ')
        {
            finish();
            continue;
        }
        // A word starts at a capital after a small letter, and numbers are words of their own
        if (!word.empty())
        {
            const char previous = word.back();
            const bool previousDigit = std::isdigit(static_cast<unsigned char>(previous)) != 0;
            if ((upper && std::islower(static_cast<unsigned char>(previous))) || digit != previousDigit)
                finish();
        }
        word += c;
    }
    finish();

    if (words.size() > 4)
        words.resize(4);
    std::string label;
    for (const auto& w : words)
        label += (label.empty() ? "" : " ") + w;
    return label;
}

// How big an object is, in units of about 1 m (100 game units): the cube root of its bounds' volume (base object
// bounds at 0x20: min x, y, z, max x, y, z), scaled
float GetSize(const TESObjectREFR* apRef) noexcept
{
    const auto* pBounds = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(apRef->baseForm) + 0x20);
    float volume = 1.f;
    for (int axis = 0; axis < 3; ++axis)
        volume *= std::max(1.f, static_cast<float>(pBounds[3 + axis] - pBounds[axis]));
    return std::cbrt(volume) * apRef->GetScale() / 100.f;
}
} // namespace

namespace ScenerySalvage
{
const char* GetLabel(const TESForm* apBase) noexcept
{
    if (!apBase)
        return nullptr;

    // By base object, kept for good: the game keeps the pointer it gets
    static std::mutex s_lock;
    static std::unordered_map<uint32_t, std::string> s_labels;

    std::scoped_lock _{s_lock};
    auto it = s_labels.find(apBase->formID);
    if (it == s_labels.end())
        it = s_labels.emplace(apBase->formID, MakeLabel(GetModel(apBase))).first;
    return it->second.empty() ? nullptr : it->second.c_str();
}

Vector<std::pair<uint32_t, uint32_t>> GetSalvage(const TESObjectREFR* apRef) noexcept
{
    Vector<std::pair<uint32_t, uint32_t>> salvage;
    if (!apRef || !apRef->baseForm)
        return salvage;

    const std::string model = ToLower(GetModel(apRef->baseForm));
    const std::vector<Yield>* pYields = &kOtherYields;
    for (const Kind& kind : GetKinds())
    {
        if (std::any_of(kind.Words.begin(), kind.Words.end(), [&](const char* apWord) { return model.find(apWord) != std::string::npos; }))
        {
            pYields = &kind.Yields;
            break;
        }
    }

    // At least one of what it's mostly made of, of the rest what its size gives
    const float size = GetSize(apRef);
    for (size_t i = 0; i < pYields->size(); ++i)
    {
        const long count = std::clamp(std::lround((*pYields)[i].PerSize * size), i == 0 ? 1L : 0L, 40L);
        if (count > 0)
            salvage.emplace_back((*pYields)[i].Item, static_cast<uint32_t>(count));
    }
    return salvage;
}
} // namespace ScenerySalvage

#endif
