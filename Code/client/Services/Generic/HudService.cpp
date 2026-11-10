#include <Services/HudService.h>

#if TP_FALLOUT4

#include <World.h>
#include <Events/OverlayRestartedEvent.h>
#include <Events/UpdateEvent.h>
#include <PlayerCharacter.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/BGSInventoryList.h>
#include <ExtraData/ExtraDataList.h>
#include <Interface/HudMarker.h>
#include <Services/OverlayService.h>
#include <MiniJson.h>

#include <cmath>
#include <PerfProbe.h>

namespace
{
// The settings MCM writes for our page (Data/MCM/Config/ReclamationDay/config.json), looked at this often (s)
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"HUD";
constexpr double kSettingsInterval = 2.0;
// The compass's markers are copied this often (s); the rest every frame
constexpr double kMarkersInterval = 0.05;
// Radiation takes this much of the health bar at its worst: 1000 rads (to see in game, docs/HUD.md)
constexpr float kRadsForAll = 1000.f;
// The probe: this many lines after each load, this far apart (s)
constexpr int kProbes = 12;
constexpr double kProbeInterval = 5.0;
// The game's HUD colour is read this often (s): its sliders change it in the settings while the game runs
constexpr double kColorInterval = 0.25;
// and sent to the overlay again this often unchanged, for a page that's just loaded (s)
constexpr double kColorResend = 5.0;
// The drawn weapon's name is looked up this often (s)
constexpr double kWeaponInterval = 0.25;
// TESObjectWEAP: its data at 0x198, the ammo it fires at 0x68 of that (none: a melee weapon)
constexpr uintptr_t kWeaponDataOffset = 0x198;
constexpr uintptr_t kWeaponAmmoOffset = 0x68;

// The weapon in the right hand while it's drawn: its name (the worn stack's own, as the player named it, else the
// weapon's), empty with none drawn; whether it fires ammo
std::string DrawnWeaponName(PlayerCharacter* apPlayer, bool& aHasAmmo) noexcept
{
    aHasAmmo = false;
    TESForm* pWeapon = apPlayer->actorState.IsWeaponDrawn() ? apPlayer->GetEquippedWeapon(0) : nullptr;
    if (!pWeapon)
        return {};
    aHasAmmo = *reinterpret_cast<TESForm* const*>(reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponDataOffset + kWeaponAmmoOffset) != nullptr;
    if (apPlayer->pInventoryList)
    {
        for (auto& item : apPlayer->pInventoryList->DataA)
        {
            if (item.pObject != pWeapon)
                continue;
            for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack)
            {
                if (!pStack->IsEquipped() || !pStack->spExtra)
                    continue;
                const String customName = pStack->spExtra->GetCustomName();
                if (!customName.empty())
                    return customName.c_str();
            }
        }
    }
    const char* pName = pWeapon->GetName();
    return pName ? pName : "";
}

// A setting of the game's INIPrefSettingCollection (CommonLibF4 Setting: an int's value at 0x08, the key at 0x10)
struct PrefSetting
{
    void* vtable;    // 00
    int32_t value;   // 08
    uint32_t pad0C;  // 0C
    const char* key; // 10, "name:Section"
};

struct PrefNode
{
    PrefSetting* item;
    PrefNode* next;
};

// Fallout4Prefs.ini's settings as the game has them now: INIPrefSettingCollection's singleton (CommonLibF4 id 767844),
// its list (BSSimpleList) at 0x118 like the INI collection's
PrefSetting* FindPrefSetting(const char* acpKey) noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_prefSettings, 767845);
    uint8_t* pCollection = *s_prefSettings.Get();
    if (!pCollection)
        return nullptr;
    for (auto* pNode = reinterpret_cast<PrefNode*>(pCollection + 0x118); pNode; pNode = pNode->next)
    {
        if (pNode->item && pNode->item->key && _stricmp(pNode->item->key, acpKey) == 0)
            return pNode->item;
    }
    return nullptr;
}

// The HUD colour of the game's settings (Display: HUD Color, iHUDColorR/G/B:Interface, 0 to 255), what the game's own
// HUD is drawn in
bool ReadHudColor(std::array<int, 3>& aOut) noexcept
{
    static PrefSetting* s_settings[3]{};
    constexpr const char* kKeys[3] = {"iHUDColorR:Interface", "iHUDColorG:Interface", "iHUDColorB:Interface"};
    for (int i = 0; i < 3; ++i)
    {
        if (!s_settings[i])
            s_settings[i] = FindPrefSetting(kKeys[i]);
        if (!s_settings[i])
            return false;
        aOut[i] = std::clamp(s_settings[i]->value, 0, 255);
    }
    return true;
}

float Share(float aValue, float aOf) noexcept
{
    return aOf > 0.f ? std::clamp(aValue / aOf, 0.f, 1.f) : 0.f;
}

// Where the camera looks, clockwise from north (+Y) in radians: the world to clip matrix's last row is the view's
// forward direction (HudMarker)
bool GetCameraHeading(float& aOut) noexcept
{
    float m[4][4];
    if (!HudMarker::GetWorldToClip(m))
        return false;
    if (m[3][0] == 0.f && m[3][1] == 0.f)
        return false;
    aOut = std::atan2(m[3][0], m[3][1]);
    return true;
}

std::string Number(float aValue) noexcept
{
    return std::isfinite(aValue) ? fmt::format("{:.4g}", aValue) : "0";
}
} // namespace

HudService::HudService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&HudService::OnUpdate>(this);
    m_overlayRestartedConnection = aDispatcher.sink<OverlayRestartedEvent>().connect<&HudService::OnOverlayRestarted>(this);
}

void HudService::OnOverlayRestarted(const OverlayRestartedEvent&) noexcept
{
    m_sent.clear();
    m_colorSendTimer = 0.0;
}

void HudService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Hud");
    if ((m_settingsTimer -= acEvent.Delta) <= 0.0)
    {
        m_settingsTimer = kSettingsInterval;
        LoadSettings();
    }
    if ((m_colorTimer -= acEvent.Delta) <= 0.0)
    {
        m_colorTimer = kColorInterval;
        std::array<int, 3> color{};
        const bool changed = ReadHudColor(color) && (!m_hasColor || color != m_color);
        if (changed)
        {
            spdlog::info("Game HUD: the game's HUD colour is {} {} {}", color[0], color[1], color[2]);
            m_color = color;
            m_hasColor = true;
        }
        // The whole overlay is drawn in it, its menus and windows too, not only the HUD (the game's
        // HUD Color sliders colour the whole overlay): sent as it changes, and again now and then for a page that's just loaded
        m_colorSendTimer -= kColorInterval;
        if (m_hasColor && (changed || m_colorSendTimer <= 0.0))
        {
            m_colorSendTimer = kColorResend;
            m_world.ctx().at<OverlayService>().SendEvent("hudColor", fmt::format("[{},{},{}]", m_color[0], m_color[1], m_color[2]));
        }
    }

    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
    {
        m_hadPlayer = false;
        Clear();
        return;
    }
    if (!m_hadPlayer)
    {
        m_hadPlayer = true;
        m_probesLeft = kProbes;
        m_probeTimer = kProbeInterval;
    }

    GameHud::Install();
    // With the overlay off for good (its process kept ending) the game's own HUD is back
    const bool enabled = m_settings.Enabled && !m_world.ctx().at<OverlayService>().IsOff();
    GameHud::Hide(enabled);
    if (!enabled)
    {
        Clear();
        return;
    }

    if ((m_weaponTimer -= acEvent.Delta) <= 0.0)
    {
        m_weaponTimer = kWeaponInterval;
        m_weaponName = DrawnWeaponName(pPlayer, m_weaponHasAmmo);
    }

    if ((m_markersTimer -= acEvent.Delta) <= 0.0)
    {
        m_markersTimer = kMarkersInterval;
        if (GameHud::WouldShow(GameHud::Piece::kCompass))
            GameHud::GetCompassMarkers(m_markers);
        else
            m_markers.clear();
    }

    if (m_probesLeft > 0 && (m_probeTimer -= acEvent.Delta) <= 0.0)
    {
        m_probeTimer = kProbeInterval;
        --m_probesLeft;
        Probe();
    }

    std::string state = BuildState();
    if (state != m_sent)
    {
        m_world.ctx().at<OverlayService>().SendEvent("hud", state);
        m_sent = std::move(state);
    }
}

void HudService::LoadSettings() noexcept
{
    const auto path = (std::filesystem::current_path() / L"Data" / kSettingsFile).wstring();
    wchar_t text[32]{};
    GetPrivateProfileStringW(kSettingsSection, L"bEnabled", L"1", text, static_cast<DWORD>(std::size(text)), path.c_str());
    m_settings.Enabled = std::wcstol(text, nullptr, 10) != 0;
}

std::string HudService::BuildState() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    std::string json = "{";

    // The game's HUD colour, the overlay's HUD drawn in it
    if (m_hasColor)
        json += fmt::format("\"color\":[{},{},{}],", m_color[0], m_color[1], m_color[2]);

    // The compass: the camera's heading in the markers' units (the game wraps them into -half..half: pi or 180)
    float heading = 0.f;
    if (GameHud::WouldShow(GameHud::Piece::kCompass) && GetCameraHeading(heading))
    {
        const float half = GameHud::GetCompassGlobals()[1];
        const bool degrees = half > 10.f;
        if (degrees)
            heading = heading * 180.f / 3.14159265f;
        json += fmt::format("\"compass\":{{\"heading\":{},\"half\":{},\"markers\":[", Number(heading), Number(half > 0.f ? half : 3.14159265f));
        for (size_t i = 0; i < m_markers.size(); ++i)
        {
            const auto& marker = m_markers[i];
            json += fmt::format("{}[{},{},{},{},{},{},{}]", i ? "," : "", marker.Kind, Number(marker.Heading), Number(marker.Distance), marker.Height, marker.State,
                                marker.Icon, marker.Colour);
        }
        json += "]}";
    }
    else
        json += "\"compass\":null";

    // Health with the share radiation took, AP
    const float maxHealth = pPlayer->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float rads = pPlayer->GetActorValue(ActorValueInfo::kRads);
    if (GameHud::WouldShow(GameHud::Piece::kHealth))
        json += fmt::format(",\"health\":{{\"value\":{},\"rads\":{}}}", Number(Share(pPlayer->GetActorValue(ActorValueInfo::kHealth), maxHealth)),
                            Number(Share(rads, kRadsForAll)));
    else
        json += ",\"health\":null";
    json += fmt::format(",\"radiation\":{}", GameHud::WouldShow(GameHud::Piece::kRadiation) ? "true" : "false");
    if (GameHud::WouldShow(GameHud::Piece::kActionPoints))
        json += fmt::format(",\"ap\":{{\"value\":{}}}",
                            Number(Share(pPlayer->GetActorValue(ActorValueInfo::kActionPoints), pPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints))));
    else
        json += ",\"ap\":null";

    // The enemy's bar
    const auto enemy = GameHud::WouldShow(GameHud::Piece::kEnemyHealth) ? GameHud::GetEnemyHealth() : GameHud::EnemyHealth{};
    if (enemy.Valid)
        json += fmt::format(",\"enemy\":{{\"name\":{},\"value\":{},\"optional\":{},\"hostile\":{},\"legendary\":{},\"skull\":{}}}", MiniJson::Quote(enemy.Name),
                            Number(std::clamp(enemy.Health / 100.f, 0.f, 1.f)), Number(std::clamp(enemy.Optional / 100.f, 0.f, 1.f)),
                            enemy.Hostile ? "true" : "false", enemy.Legendary ? "true" : "false", enemy.Skull ? "true" : "false");
    else
        json += ",\"enemy\":null";

    // The drawn weapon's name over its ammo, both shown while it's drawn and the HUD is up (the game's counter only shows
    // for a while after a shot, a reload or a draw); grenades as the game sent them
    const bool hudUp = GameHud::WouldShow(GameHud::Piece::kCompass) || GameHud::WouldShow(GameHud::Piece::kHealth);
    const bool drawn = hudUp && !m_weaponName.empty();
    json += drawn ? fmt::format(",\"weapon\":{}", MiniJson::Quote(m_weaponName)) : std::string(",\"weapon\":null");
    const auto ammo = GameHud::WouldShow(GameHud::Piece::kAmmo) || (drawn && m_weaponHasAmmo) ? GameHud::GetAmmo() : GameHud::Counts{};
    if (ammo.Valid)
        json += fmt::format(",\"ammo\":{{\"first\":{},\"second\":{}}}", ammo.First, ammo.Second);
    else
        json += ",\"ammo\":null";
    const auto explosives = GameHud::WouldShow(GameHud::Piece::kExplosives) ? GameHud::GetExplosives() : GameHud::Counts{};
    if (explosives.Valid)
        json += fmt::format(",\"explosives\":{{\"first\":{},\"second\":{}}}", explosives.First, explosives.Second);
    else
        json += ",\"explosives\":null";

    return json + "}";
}

void HudService::Probe() noexcept
{
    auto* pPlayer = PlayerCharacter::Get();
    float heading = 0.f;
    const bool hasHeading = GetCameraHeading(heading);
    const auto globals = GameHud::GetCompassGlobals();
    const auto position = pPlayer->position;
    spdlog::info("HUD probe: at {:.0f} {:.0f} {:.0f}, camera heading {} rad ({}), compass globals {} {}; shown: compass {} health {} rads {} ap {} enemy {} "
                 "ammo {} explosives {}",
                 position.x, position.y, position.z, heading, hasHeading, globals[0], globals[1], GameHud::WouldShow(GameHud::Piece::kCompass),
                 GameHud::WouldShow(GameHud::Piece::kHealth), GameHud::WouldShow(GameHud::Piece::kRadiation), GameHud::WouldShow(GameHud::Piece::kActionPoints),
                 GameHud::WouldShow(GameHud::Piece::kEnemyHealth), GameHud::WouldShow(GameHud::Piece::kAmmo), GameHud::WouldShow(GameHud::Piece::kExplosives));
    spdlog::info("HUD probe: health {}/{}, rads {}, rad health max {}, ap {}/{}", pPlayer->GetActorValue(ActorValueInfo::kHealth),
                 pPlayer->GetActorPermanentValue(ActorValueInfo::kHealth), pPlayer->GetActorValue(ActorValueInfo::kRads),
                 pPlayer->GetActorValue(ActorValueInfo::kRadHealthMax), pPlayer->GetActorValue(ActorValueInfo::kActionPoints),
                 pPlayer->GetActorPermanentValue(ActorValueInfo::kActionPoints));
    const auto ammo = GameHud::GetAmmo();
    const auto explosives = GameHud::GetExplosives();
    const auto enemy = GameHud::GetEnemyHealth();
    spdlog::info("HUD probe: ammo {} {} {}, explosives {} {} {}, enemy {} '{}' {} {} {}{}{}", ammo.Valid, ammo.First, ammo.Second, explosives.Valid, explosives.First,
                 explosives.Second, enemy.Valid, enemy.Name, enemy.Health, enemy.Optional, enemy.Hostile ? 'H' : '-', enemy.Legendary ? 'L' : '-', enemy.Skull ? 'S' : '-');
    spdlog::info("HUD probe: {} compass markers", m_markers.size());
    for (size_t i = 0; i < m_markers.size() && i < 8; ++i)
    {
        const auto& marker = m_markers[i];
        float floats[8];
        std::memcpy(floats, marker.Raw.data(), sizeof(floats));
        uint64_t at10 = 0;
        std::memcpy(&at10, marker.Raw.data() + 0x10, sizeof(at10));
        spdlog::info("HUD probe:  kind {} icon {} state {} height {} colour {} heading {} distance {} | 00-0C {} {} {} {} | 10 {:#x} | 1C {} | 34 {}", marker.Kind,
                     marker.Icon, marker.State, marker.Height, marker.Colour, marker.Heading, marker.Distance, floats[0], floats[1], floats[2], floats[3], at10,
                     floats[7], marker.Raw[0x34]);
    }
}

void HudService::Clear() noexcept
{
    m_markers.clear();
    if (!m_sent.empty() && m_sent != "null")
    {
        m_world.ctx().at<OverlayService>().SendEvent("hud", "null");
        m_sent = "null";
    }
}

#endif
