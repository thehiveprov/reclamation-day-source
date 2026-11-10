#include <Services/ActorBarsService.h>

#if TP_FALLOUT4

#include <World.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <PlayerCharacter.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESNPC.h>
#include <Games/TES.h>
#include <Interface/UI.h>
#include <Interface/IMenu.h>
#include <Interface/Scaleform.h>
#include <Interface/HudMarker.h>
#include <Services/PartyService.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerLeft.h>
#include <WorldRays.h>

#include <array>
#include <mutex>
#include <optional>
#include <PerfProbe.h>

namespace
{
// Fallout4.esm's EncTypeLegendary, the keyword its default object LegendaryNPCKeyword_DO names
constexpr uint32_t kLegendaryKeyword = 0x001CF26C;
// The skull over characters this many levels above the player, as the HUD's own enemy meter has it; Fallout4.esm leaves
// the setting at the game's 10
constexpr char kSkullSetting[] = "iHUDEnemyHealthSkullIconLevelDifference";
constexpr int32_t kSkullDefault = 10;
// Actor Widgets' builds (the original and its 1.10.163 port): with either loaded, ours stay off
constexpr const wchar_t* kActorWidgetsDlls[] = {L"ActorWidgetsRD.dll", L"ActorWidgets.dll"};

// The settings MCM writes for our page (Data/MCM/Config/ReclamationDay/config.json), looked at this often (s)
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr float kSettingsInterval = 1.f;
// Who could have a bar, of what kind, behind a wall or not: looked at this often (s), out to the farthest bar and this
// much beyond
constexpr float kRefreshInterval = 0.25f;
constexpr float kRefreshMargin = 300.f;

constexpr float kFadeIn = 0.15f;
constexpr float kFadeOut = 0.3f;
// the health lost stays this long (s) before it shrinks
constexpr float kTrailHold = 0.2f;
constexpr float kFullHealth = 0.999f;

// The bar is this far over the head's node, or this far over the top of the character's 3D without one (times its
// scale). The vanilla skeletons' head node, then the creatures'.
constexpr float kOverHead = 24.f;
constexpr float kOverBounds = 8.f;
constexpr const char* kHeadNodes[] = {"NPC Head [Head]", "Head"};
// NiAVObject (CommonLibF4): the world transform's translation, the world bound (centre, then radius at 0x0C). Not
// TESObjectREFR::GetBoundMax/GetHeight, whose slots are Skyrim's (on Fallout 4: GetTESModel, GetVisualsRace).
constexpr size_t kWorldTranslate = 0xA0;
constexpr size_t kWorldBound = 0xB0;
// no bar for a character this close to the camera
constexpr float kMinDistance = 40.f;
// aimed at: the middle of the view goes through the character, this wide, from its feet to this far over its head node
constexpr float kAimRadius = 32.f;
constexpr float kAimOverHead = 12.f;
// the wall ray stops this short of the head: what's there is the character itself
constexpr float kWallSlack = 40.f;

// ActorState::flags2 bits 14-17, the gun state (CommonLibF4 GUN_STATE): kSighted, kFireSighted
constexpr uint32_t kGunSighted = 6;
constexpr uint32_t kGunFireSighted = 8;

// Menus the bars hide under (Actor Widgets' list; the HUD itself is gone under most others)
constexpr const char* kHidingMenus[] = {"LoadingMenu", "MainMenu",    "PauseMenu",   "PipboyMenu",    "TerminalMenu", "BookMenu",          "FavoritesMenu",
                                        "SleepWaitMenu", "LevelUpMenu", "DialogueMenu", "LooksMenu",     "SPECIALMenu",  "HolotapeMenu",      "CookingMenu",
                                        "RobotModMenu",  "PowerArmorModMenu", "ContainerMenu", "BarterMenu", "ExamineMenu"};

// The look, on the HUD's 1280 x 720 stage at scale 1
constexpr double kBarWidth = 100.;
constexpr double kHealthHeight = 6.;
constexpr double kApHeight = 3.;
constexpr double kBarGap = 2.;
constexpr double kTextSize = 15.;
// a text field's gutter around its text
constexpr double kTextGutter = 2.;
constexpr double kIconSize = 11.;
constexpr double kIconGap = 4.;
// the empty part of a bar, the health lost a moment ago
constexpr double kBackAlpha = 0.3;
constexpr double kTrailAlpha = 0.6;
// A bar's colour: the HUD's own, the game's warning colour (enemies: red, as the game's own HUD warns), or green
// (Green Friendly Bars). Each is drawn in white, and its shader target multiplies it to the colour (as the game's own HUD
// is drawn): the gameplay HUD colour type 2, the warning colour type 5 (what the game's SetToHUDColor(true) uses), or
// green fixed on the target (type 7). Type 0, which green used, is black, and drawing in the colour itself squared it
enum class Colour : uint8_t
{
    kHud,
    kWarning,
    kGreen,
};
constexpr uint32_t kDrawnColour = 0xFFFFFF;
constexpr uint32_t kGreenColour = 0x4CE65C;

uint8_t* MakeBarTarget(const GFx::Value& acRoot, Colour aColour) noexcept
{
    uint8_t* pTarget = HudMarker::MakeShaderTarget(acRoot, aColour == Colour::kWarning ? 5 : 2);
    if (pTarget && aColour == Colour::kGreen)
        HudMarker::SetShaderTargetColour(pTarget, kGreenColour);
    return pTarget;
}
constexpr size_t kMaxSprites = 100;

// --- the HUD side

// A bar as the game's update wants it
struct Bar
{
    uint32_t Id{0};
    // the line over the bars, empty when it's hidden (far away)
    std::string Text;
    bool Star{false};
    bool Skull{false};
    bool HealthBar{true};
    float Health{1.f};
    float Trail{1.f};
    // below 0: no AP bar
    float Ap{-1.f};
    Colour Tint{Colour::kHud};
    // on the screen, 0..1 left to right and top to bottom
    float X{0.f};
    float Y{0.f};
    float Scale{1.f};
    float Alpha{1.f};
    float Distance{0.f};
};

GFx::Value GetGraphics(GFx::Value& aSprite) noexcept
{
    GFx::Value graphics;
    if (!aSprite.GetMember("graphics", &graphics) || !graphics.IsObject())
        return GFx::Value();
    return graphics;
}

void BeginFill(GFx::Value& aGraphics, uint32_t aColour, double aAlpha) noexcept
{
    aGraphics.Invoke("clear");
    // no outline, only the fill
    aGraphics.Invoke("lineStyle");
    const GFx::Value args[] = {GFx::Value(static_cast<double>(aColour)), GFx::Value(aAlpha)};
    aGraphics.Invoke("beginFill", nullptr, args, 2);
}

void Polygon(GFx::Value& aGraphics, const std::vector<std::pair<double, double>>& acPoints) noexcept
{
    for (size_t i = 0; i <= acPoints.size(); ++i)
    {
        const auto& [x, y] = acPoints[i % acPoints.size()];
        const GFx::Value args[] = {GFx::Value(x), GFx::Value(y)};
        aGraphics.Invoke(i == 0 ? "moveTo" : "lineTo", nullptr, args, 2);
    }
}

// A bar: a rectangle from (0, 0), scaled along x for how full it is
void DrawRect(GFx::Value& aSprite, uint32_t aColour, double aAlpha, double aHeight) noexcept
{
    GFx::Value graphics = GetGraphics(aSprite);
    if (!graphics.IsObject())
        return;
    BeginFill(graphics, aColour, aAlpha);
    const GFx::Value args[] = {GFx::Value(0.), GFx::Value(0.), GFx::Value(kBarWidth), GFx::Value(aHeight)};
    graphics.Invoke("drawRect", nullptr, args, 4);
    graphics.Invoke("endFill");
}

// The legendary star, five points round its middle
void DrawStar(GFx::Value& aSprite, uint32_t aColour) noexcept
{
    GFx::Value graphics = GetGraphics(aSprite);
    if (!graphics.IsObject())
        return;
    BeginFill(graphics, aColour, 1.);
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 10; ++i)
    {
        const double angle = -1.5707963 + i * 0.6283185;
        const double radius = (i % 2 ? 0.4 : 1.) * kIconSize * 0.55;
        points.emplace_back(std::cos(angle) * radius, std::sin(angle) * radius);
    }
    Polygon(graphics, points);
    graphics.Invoke("endFill");
}

// The skull: the head and jaw in one shape, the eyes and nose holes in it (the drawing's fill is even-odd)
void DrawSkull(GFx::Value& aSprite, uint32_t aColour) noexcept
{
    GFx::Value graphics = GetGraphics(aSprite);
    if (!graphics.IsObject())
        return;
    BeginFill(graphics, aColour, 1.);
    const double unit = kIconSize / 12.;
    std::vector<std::pair<double, double>> outline;
    // the top of the head, from the left cheek round to the right one
    for (int i = 0; i <= 8; ++i)
    {
        const double angle = 2.79 + i * (3.84 / 8.);
        outline.emplace_back(std::cos(angle) * 5.6 * unit, (-1.2 + std::sin(angle) * 5.6) * unit);
    }
    for (const auto& [x, y] : {std::pair{3.2, 3.3}, {3.2, 6.}, {-3.2, 6.}, {-3.2, 3.3}})
        outline.emplace_back(x * unit, y * unit);
    Polygon(graphics, outline);
    for (const double side : {-2.2, 2.2})
    {
        const GFx::Value args[] = {GFx::Value(side * unit), GFx::Value(-0.8 * unit), GFx::Value(1.6 * unit)};
        graphics.Invoke("drawCircle", nullptr, args, 3);
    }
    Polygon(graphics, {{0., 1.4 * unit}, {0.9 * unit, 3. * unit}, {-0.9 * unit, 3. * unit}});
    graphics.Invoke("endFill");
}

void SetTextColour(GFx::Movie* apMovie, GFx::Value& aText, uint32_t aColour) noexcept
{
    GFx::Value format;
    apMovie->CreateObject(&format, "flash.text.TextFormat");
    if (!format.IsObject())
        return;
    // the game's own fonts (Interface/fonts_en.swf, fontconfig.txt)
    format.SetMember("font", GFx::Value("$MAIN_Font_Bold"));
    format.SetMember("size", GFx::Value(kTextSize));
    format.SetMember("color", GFx::Value(static_cast<double>(aColour)));
    aText.SetMember("defaultTextFormat", format);
}

// The bars in the HUD movie, as its frame puts them there
class BarsLayer final : public HudMarker::Layer
{
public:
    void SetWanted(std::vector<Bar>&& aBars) noexcept
    {
        std::scoped_lock _(m_wantedLock);
        m_wanted = std::move(aBars);
    }

    void Advance(IMenu* apHud) noexcept override;
    void AppendShaderFXInfos(IMenu* apHud, void* apBackground, void* apForeground) noexcept override;
    void TearDown(IMenu* apHud) noexcept override;

private:
    // One bar's display objects, all in its root: the root is one of the HUD's shader targets, drawn with the HUD's
    // backing
    struct Sprites
    {
        uint32_t id{0};
        bool used{false};
        GFx::Value root;
        GFx::Value text;
        GFx::Value star;
        GFx::Value skull;
        GFx::Value healthBack;
        GFx::Value healthTrail;
        GFx::Value healthFill;
        GFx::Value apBack;
        GFx::Value apFill;
        uint8_t* pTarget{nullptr};
        Colour colour{Colour::kHud};

        // what the movie has, so that only changes go to it
        bool visible{false};
        std::string text_;
        double textWidth{0.};
        double textHeight{0.};
        int layout{-1};
        double x{0.};
        double y{0.};
        double scale{-1.};
        double alpha{-1.};
        double health{-1.};
        double trail{-1.};
        double ap{-1.};

        std::array<GFx::Value*, 9> Values() noexcept { return {&root, &text, &star, &skull, &healthBack, &healthTrail, &healthFill, &apBack, &apFill}; }
    };

    bool Build(GFx::Movie* apMovie) noexcept;
    std::unique_ptr<Sprites> MakeSprites(GFx::Movie* apMovie, Colour aColour) noexcept;
    void Paint(GFx::Movie* apMovie, Sprites& aSprites, Colour aColour) noexcept;
    void Recolour(GFx::Movie* apMovie, Sprites& aSprites, Colour aColour) noexcept;
    void Apply(GFx::Movie* apMovie, Sprites& aSprites, const Bar& acBar, double aLeft, double aTop, double aRight, double aBottom) noexcept;
    void Layout(Sprites& aSprites, const Bar& acBar) noexcept;
    void Hide(Sprites& aSprites) noexcept;
    // The movie went without TearDown: its values are dropped untouched, and its targets with them
    void Forget() noexcept;

    std::mutex m_wantedLock;
    std::vector<Bar> m_wanted;

    // the HUD's frame only
    GFx::Movie* m_pMovie{nullptr};
    GFx::Value m_group;
    GFx::Value m_container;
    std::vector<std::unique_ptr<Sprites>> m_sprites;

    // the shader targets, also read by the HUD's drawing
    std::mutex m_targetsLock;
    std::vector<uint8_t*> m_targets;
};

BarsLayer* s_pLayer = nullptr;

bool BarsLayer::Build(GFx::Movie* apMovie) noexcept
{
    m_pMovie = apMovie;
    // in the crosshair's group, as the marker is, under the rest of it
    if (!apMovie->GetVariable(&m_group, "root.CenterGroup_mc") || !m_group.IsObject())
        return false;
    apMovie->CreateObject(&m_container, "flash.display.Sprite");
    if (!m_container.IsObject())
        return false;
    m_container.SetMember("mouseEnabled", GFx::Value(false));
    m_container.SetMember("mouseChildren", GFx::Value(false));
    const GFx::Value args[] = {m_container, GFx::Value(0.)};
    m_group.Invoke("addChildAt", nullptr, args, 2);
    spdlog::info("Actor bars: made in the HUD, colour {:06X}", HudMarker::GetHudColour());
    return true;
}

void BarsLayer::Paint(GFx::Movie* apMovie, Sprites& aSprites, Colour aColour) noexcept
{
    const uint32_t colour = kDrawnColour;
    DrawRect(aSprites.healthBack, colour, kBackAlpha, kHealthHeight);
    DrawRect(aSprites.healthTrail, colour, kTrailAlpha, kHealthHeight);
    DrawRect(aSprites.healthFill, colour, 1., kHealthHeight);
    DrawRect(aSprites.apBack, colour, kBackAlpha, kApHeight);
    DrawRect(aSprites.apFill, colour, 1., kApHeight);
    DrawStar(aSprites.star, colour);
    DrawSkull(aSprites.skull, colour);
    SetTextColour(apMovie, aSprites.text, colour);
    // the text already there takes the new format when it's set again
    aSprites.text.SetMember("text", GFx::Value(aSprites.text_.c_str()));
    aSprites.colour = aColour;
}

std::unique_ptr<BarsLayer::Sprites> BarsLayer::MakeSprites(GFx::Movie* apMovie, Colour aColour) noexcept
{
    auto pSprites = std::make_unique<Sprites>();
    Sprites& sprites = *pSprites;
    apMovie->CreateObject(&sprites.root, "flash.display.Sprite");
    if (!sprites.root.IsObject())
        return nullptr;
    sprites.root.SetMember("mouseEnabled", GFx::Value(false));
    sprites.root.SetMember("mouseChildren", GFx::Value(false));
    sprites.root.SetMember("visible", GFx::Value(false));

    for (GFx::Value* pSprite : {&sprites.healthBack, &sprites.healthTrail, &sprites.healthFill, &sprites.apBack, &sprites.apFill, &sprites.star, &sprites.skull})
    {
        apMovie->CreateObject(pSprite, "flash.display.Sprite");
        if (!pSprite->IsObject())
            return nullptr;
        sprites.root.Invoke("addChild", nullptr, pSprite, 1);
    }
    for (GFx::Value* pBar : {&sprites.healthBack, &sprites.healthTrail, &sprites.healthFill, &sprites.apBack, &sprites.apFill})
        pBar->SetMember("x", GFx::Value(-kBarWidth * 0.5));

    apMovie->CreateObject(&sprites.text, "flash.text.TextField");
    if (!sprites.text.IsObject())
        return nullptr;
    sprites.text.SetMember("embedFonts", GFx::Value(true));
    sprites.text.SetMember("selectable", GFx::Value(false));
    sprites.text.SetMember("mouseEnabled", GFx::Value(false));
    sprites.text.SetMember("autoSize", GFx::Value("left"));
    sprites.root.Invoke("addChild", nullptr, &sprites.text, 1);

    Paint(apMovie, sprites, aColour);
    m_container.Invoke("addChild", nullptr, &sprites.root, 1);
    sprites.pTarget = MakeBarTarget(sprites.root, aColour);
    if (sprites.pTarget)
    {
        std::scoped_lock _(m_targetsLock);
        m_targets.push_back(sprites.pTarget);
    }
    return pSprites;
}

// The colour type is the target's own: a bar that changes colour gets a new target
void BarsLayer::Recolour(GFx::Movie* apMovie, Sprites& aSprites, Colour aColour) noexcept
{
    Paint(apMovie, aSprites, aColour);
    uint8_t* pOld = aSprites.pTarget;
    aSprites.pTarget = MakeBarTarget(aSprites.root, aColour);
    {
        std::scoped_lock _(m_targetsLock);
        std::erase(m_targets, pOld);
        if (aSprites.pTarget)
            m_targets.push_back(aSprites.pTarget);
    }
    HudMarker::FreeShaderTarget(pOld);
}

// Where the parts go under the anchor (the bar's root): the AP bar at the bottom, the health bar over it, the name line
// over that with the star before it and the skull after it
void BarsLayer::Layout(Sprites& aSprites, const Bar& acBar) noexcept
{
    const bool ap = acBar.Ap >= 0.f;
    const bool text = !acBar.Text.empty();
    const int layout = (ap ? 1 : 0) | (acBar.HealthBar ? 2 : 0) | (text ? 4 : 0) | (acBar.Star ? 8 : 0) | (acBar.Skull ? 16 : 0);
    if (layout == aSprites.layout)
        return;
    aSprites.layout = layout;

    double bottom = 0.;
    for (GFx::Value* pBar : {&aSprites.apBack, &aSprites.apFill})
    {
        pBar->SetMember("visible", GFx::Value(ap));
        pBar->SetMember("y", GFx::Value(-kApHeight));
    }
    if (ap)
        bottom -= kApHeight + kBarGap;
    for (GFx::Value* pBar : {&aSprites.healthBack, &aSprites.healthTrail, &aSprites.healthFill})
    {
        pBar->SetMember("visible", GFx::Value(acBar.HealthBar));
        pBar->SetMember("y", GFx::Value(bottom - kHealthHeight));
    }
    if (acBar.HealthBar)
        bottom -= kHealthHeight + kBarGap;

    aSprites.text.SetMember("visible", GFx::Value(text));
    aSprites.star.SetMember("visible", GFx::Value(text && acBar.Star));
    aSprites.skull.SetMember("visible", GFx::Value(text && acBar.Skull));
    if (!text)
        return;
    const double width = aSprites.textWidth + 2. * kTextGutter;
    const double height = aSprites.textHeight + 2. * kTextGutter;
    const double left = -width * 0.5;
    const double top = bottom - height + kTextGutter;
    aSprites.text.SetMember("x", GFx::Value(left));
    aSprites.text.SetMember("y", GFx::Value(top));
    const double middle = top + height * 0.5;
    aSprites.star.SetMember("x", GFx::Value(left + kTextGutter - kIconGap - kIconSize * 0.5));
    aSprites.star.SetMember("y", GFx::Value(middle));
    aSprites.skull.SetMember("x", GFx::Value(left + width - kTextGutter + kIconGap + kIconSize * 0.5));
    aSprites.skull.SetMember("y", GFx::Value(middle));
}

void BarsLayer::Apply(GFx::Movie* apMovie, Sprites& aSprites, const Bar& acBar, double aLeft, double aTop, double aRight, double aBottom) noexcept
{
    if (aSprites.colour != acBar.Tint)
        Recolour(apMovie, aSprites, acBar.Tint);

    if (acBar.Text != aSprites.text_)
    {
        aSprites.text_ = acBar.Text;
        aSprites.text.SetMember("text", GFx::Value(aSprites.text_.c_str()));
        GFx::Value width;
        GFx::Value height;
        aSprites.textWidth = aSprites.text.GetMember("textWidth", &width) ? width.GetNumber() : 0.;
        aSprites.textHeight = aSprites.text.GetMember("textHeight", &height) ? height.GetNumber() : kTextSize;
        aSprites.layout = -1;
    }
    Layout(aSprites, acBar);

    const auto set = [](GFx::Value& aObject, const char* acpName, double aValue, double& aShown, double aStep)
    {
        if (std::abs(aValue - aShown) < aStep)
            return;
        aShown = aValue;
        aObject.SetMember(acpName, GFx::Value(aValue));
    };
    set(aSprites.root, "x", aLeft + acBar.X * (aRight - aLeft), aSprites.x, 0.05);
    set(aSprites.root, "y", aTop + acBar.Y * (aBottom - aTop), aSprites.y, 0.05);
    const double scale = aSprites.scale;
    set(aSprites.root, "scaleX", acBar.Scale, aSprites.scale, 0.002);
    if (aSprites.scale != scale)
        aSprites.root.SetMember("scaleY", GFx::Value(aSprites.scale));
    set(aSprites.root, "alpha", acBar.Alpha, aSprites.alpha, 0.01);
    set(aSprites.healthFill, "scaleX", acBar.Health, aSprites.health, 0.001);
    set(aSprites.healthTrail, "scaleX", acBar.Trail, aSprites.trail, 0.001);
    if (acBar.Ap >= 0.f)
        set(aSprites.apFill, "scaleX", acBar.Ap, aSprites.ap, 0.001);

    if (!aSprites.visible)
    {
        aSprites.visible = true;
        aSprites.root.SetMember("visible", GFx::Value(true));
    }
}

void BarsLayer::Hide(Sprites& aSprites) noexcept
{
    if (!aSprites.visible)
        return;
    aSprites.visible = false;
    aSprites.root.SetMember("visible", GFx::Value(false));
}

void BarsLayer::Forget() noexcept
{
    {
        std::scoped_lock _(m_targetsLock);
        m_targets.clear();
    }
    for (auto& pSprites : m_sprites)
    {
        for (GFx::Value* pValue : pSprites->Values())
            pValue->Forget();
    }
    m_sprites.clear();
    m_container.Forget();
    m_group.Forget();
    m_pMovie = nullptr;
}

void BarsLayer::Advance(IMenu* apHud) noexcept
{
    auto* pMovie = static_cast<GFx::Movie*>(apHud->pMovie);
    if (!pMovie)
        return;

    std::vector<Bar> wanted;
    {
        std::scoped_lock _(m_wantedLock);
        wanted = m_wanted;
    }

    // made once per HUD movie (a new game or a load can make a new one)
    if (pMovie != m_pMovie)
    {
        if (m_pMovie)
            Forget();
        if (wanted.empty())
            return;
        if (!Build(pMovie))
        {
            m_container = GFx::Value();
            m_group = GFx::Value();
            return;
        }
    }
    if (!m_container.IsObject())
        return;

    // the screen in the container's space
    double left = 0.;
    double top = 0.;
    double right = 0.;
    double bottom = 0.;
    if (wanted.empty() || !HudMarker::ScreenToLocal(pMovie, m_container, 0.f, 0.f, left, top) || !HudMarker::ScreenToLocal(pMovie, m_container, 1.f, 1.f, right, bottom))
    {
        for (auto& pSprites : m_sprites)
            Hide(*pSprites);
        return;
    }

    // a bar keeps its sprites; the others' are free for new bars
    for (auto& pSprites : m_sprites)
        pSprites->used = false;
    std::vector<Sprites*> assigned(wanted.size(), nullptr);
    for (size_t i = 0; i < wanted.size(); ++i)
    {
        for (auto& pSprites : m_sprites)
        {
            if (!pSprites->used && pSprites->id == wanted[i].Id)
            {
                pSprites->used = true;
                assigned[i] = pSprites.get();
                break;
            }
        }
    }
    for (size_t i = 0; i < wanted.size(); ++i)
    {
        if (assigned[i])
            continue;
        Sprites* pFree = nullptr;
        for (auto& pSprites : m_sprites)
        {
            if (!pSprites->used)
            {
                pFree = pSprites.get();
                break;
            }
        }
        if (!pFree && m_sprites.size() < kMaxSprites)
        {
            if (auto pNew = MakeSprites(pMovie, wanted[i].Tint))
            {
                m_sprites.push_back(std::move(pNew));
                pFree = m_sprites.back().get();
            }
        }
        if (!pFree)
            continue;
        pFree->used = true;
        // another character's: what the movie has is still right, the shown values aren't its
        if (pFree->id != wanted[i].Id)
        {
            pFree->id = wanted[i].Id;
            pFree->health = pFree->trail = pFree->ap = -1.;
        }
        assigned[i] = pFree;
    }

    for (auto& pSprites : m_sprites)
    {
        if (!pSprites->used)
            Hide(*pSprites);
    }
    for (size_t i = 0; i < wanted.size(); ++i)
    {
        if (assigned[i])
            Apply(pMovie, *assigned[i], wanted[i], left, top, right, bottom);
    }
}

void BarsLayer::AppendShaderFXInfos(IMenu* apHud, void* apBackground, void* apForeground) noexcept
{
    if (!apHud || !m_pMovie || apHud->pMovie != m_pMovie)
        return;
    std::scoped_lock _(m_targetsLock);
    for (uint8_t* pTarget : m_targets)
        HudMarker::AppendShaderTarget(pTarget, apBackground, apForeground);
}

// Out of a HUD that's going away while its movie is still there. The targets' destructors take them off the HUD
// colour events (left on them, the next HUD's opening would send them one that reads this movie, freed by then).
void BarsLayer::TearDown(IMenu* apHud) noexcept
{
    if (!apHud || !m_pMovie || apHud->pMovie != m_pMovie)
        return;
    {
        std::scoped_lock _(m_targetsLock);
        m_targets.clear();
    }
    for (auto& pSprites : m_sprites)
    {
        HudMarker::FreeShaderTarget(pSprites->pTarget);
        pSprites->pTarget = nullptr;
    }
    if (m_group.IsObject() && m_container.IsObject())
        m_group.Invoke("removeChild", nullptr, &m_container, 1);
    m_sprites.clear();
    m_container = GFx::Value();
    m_group = GFx::Value();
    m_pMovie = nullptr;
}

// --- the game side

bool IsMenuOpen(const BSFixedString& acMenu) noexcept
{
    UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(acMenu);
}

bool IsHiddenByMenu() noexcept
{
    static const auto s_menus = []
    {
        std::vector<std::unique_ptr<BSFixedString>> menus;
        for (const char* pName : kHidingMenus)
            menus.push_back(std::make_unique<BSFixedString>(pName));
        return menus;
    }();
    return std::any_of(s_menus.begin(), s_menus.end(), [](const auto& acpMenu) { return IsMenuOpen(*acpMenu); });
}

const BSFixedString& HeadNodeName(int aIndex) noexcept
{
    static const auto s_names = []
    {
        std::vector<std::unique_ptr<BSFixedString>> names;
        for (const char* pName : kHeadNodes)
            names.push_back(std::make_unique<BSFixedString>(pName));
        return names;
    }();
    return *s_names[aIndex];
}

uint8_t* GetObjectByName(uint8_t* apNode, const BSFixedString& acName) noexcept
{
    using TGetObjectByName = uint8_t* (*)(uint8_t*, const BSFixedString&);
    return (*reinterpret_cast<TGetObjectByName* const*>(apNode))[0x2E](apNode, acName);
}

// Actor::GetHostileToActor (CommonLibF4 id 1148686)
bool IsHostileTo(Actor* apActor, Actor* apOther) noexcept
{
    using TGetHostileToActor = bool(Actor*, Actor*);
    POINTER_FALLOUT4(TGetHostileToActor, s_getHostileToActor, 1148687);
    return s_getHostileToActor.Get()(apActor, apOther);
}

// TESObjectREFR::HasKeywordHelper (its vtable's slot 0xC5): the reference's keywords, its base's and race's included
bool HasKeyword(const Actor* apActor, const void* apKeyword) noexcept
{
    using THasKeyword = bool(const Actor*, const void*, const void*);
    return (*reinterpret_cast<THasKeyword* const* const*>(apActor))[0xC5](apActor, apKeyword, nullptr);
}

// A game setting's integer value. GameSettingCollection (CommonLibF4 id 8308) keeps its settings in a BSTBTree at 0x118,
// its root node at 0x128; a node holds up to 4 entries (a name, then the Setting) and 5 child nodes at 0x40, the count
// of entries used at 0x68. A Setting's value is at 0x08, its name at 0x10.
std::optional<int32_t> FindGameSettingInt(const char* acpName) noexcept
{
    POINTER_FALLOUT4(const uint8_t*, s_collection, 8309);
    const uint8_t* pCollection = *s_collection.Get();
    if (!pCollection)
        return std::nullopt;

    std::vector<const uint8_t*> pending{*reinterpret_cast<const uint8_t* const*>(pCollection + 0x128)};
    for (size_t visited = 0; !pending.empty() && visited < 20000; ++visited)
    {
        const uint8_t* pNode = pending.back();
        pending.pop_back();
        if (!pNode)
            continue;
        const uint32_t used = *reinterpret_cast<const uint32_t*>(pNode + 0x68);
        if (used > 4)
            return std::nullopt;
        for (uint32_t i = 0; i < used; ++i)
        {
            const uint8_t* pSetting = *reinterpret_cast<const uint8_t* const*>(pNode + i * 0x10 + 0x08);
            const char* pName = pSetting ? *reinterpret_cast<const char* const*>(pSetting + 0x10) : nullptr;
            if (pName && _stricmp(pName, acpName) == 0)
                return *reinterpret_cast<const int32_t*>(pSetting + 0x08);
        }
        for (uint32_t i = 0; i <= used; ++i)
            pending.push_back(*reinterpret_cast<const uint8_t* const*>(pNode + 0x40 + i * 8));
    }
    return std::nullopt;
}

// The name on the character's record (TESActorBase's full name). Not TESForm::GetName: on Fallout 4 its slot (0x35) is
// GetTextForParsedSubTag, which reads a string it was never given (a crash reading 0xF, 2026-10-21)
std::string NameOf(const Actor* apActor) noexcept
{
    const auto* pNpc = Cast<TESNPC>(apActor->baseForm);
    const char* pName = pNpc ? pNpc->fullName.value.AsAscii() : nullptr;
    return pName ? pName : "";
}

std::string Uppercase(std::string aText) noexcept
{
    // ASCII letters only: the bytes of other UTF-8 letters are left as they are
    for (char& c : aText)
    {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }
    return aText;
}

// An MCM settings file: values MCM wrote over our defaults
struct IniFile
{
    std::wstring path;

    [[nodiscard]] bool Bool(const wchar_t* acpSection, const wchar_t* acpKey, bool aDefault) const noexcept { return Int(acpSection, acpKey, aDefault ? 1 : 0) != 0; }
    [[nodiscard]] int Int(const wchar_t* acpSection, const wchar_t* acpKey, int aDefault) const noexcept
    {
        return static_cast<int>(Float(acpSection, acpKey, static_cast<float>(aDefault)));
    }
    [[nodiscard]] float Float(const wchar_t* acpSection, const wchar_t* acpKey, float aDefault) const noexcept
    {
        wchar_t text[32]{};
        GetPrivateProfileStringW(acpSection, acpKey, L"", text, static_cast<DWORD>(std::size(text)), path.c_str());
        wchar_t* pEnd = nullptr;
        const float value = std::wcstof(text, &pEnd);
        return pEnd != text && std::isfinite(value) ? value : aDefault;
    }

    void Read(const wchar_t* acpSection, ActorBarsService::Rules& aRules) const noexcept
    {
        aRules.Show = Bool(acpSection, L"bShow", aRules.Show);
        aRules.AlwaysDisplay = Bool(acpSection, L"bAlwaysDisplay", aRules.AlwaysDisplay);
        aRules.CombatOnly = Bool(acpSection, L"bCombatOnly", aRules.CombatOnly);
        aRules.MissingHealthOnly = Bool(acpSection, L"bMissingHealthOnly", aRules.MissingHealthOnly);
        aRules.AimedTargetOnly = Bool(acpSection, L"bAimedTargetOnly", aRules.AimedTargetOnly);
        aRules.HideBehindWalls = Bool(acpSection, L"bHideBehindWalls", aRules.HideBehindWalls);
        aRules.LegendaryStar = Bool(acpSection, L"bShowLegendaryIcon", aRules.LegendaryStar);
        aRules.Skull = Bool(acpSection, L"bShowBossMarker", aRules.Skull);
    }
};

std::filesystem::path SettingsPath() noexcept
{
    // the game's folder, as the script extender is found (ScriptExtender.cpp)
    return std::filesystem::current_path() / L"Data" / kSettingsFile;
}
} // namespace

ActorBarsService::ActorBarsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ActorBarsService::OnUpdate>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&ActorBarsService::OnPlayerJoined>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&ActorBarsService::OnPlayerLevel>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&ActorBarsService::OnPlayerLeft>(this);
    // the HUD's side lives as long as the game (HudMarker keeps it)
    if (!s_pLayer)
    {
        s_pLayer = new BarsLayer();
        HudMarker::AddLayer(s_pLayer);
    }
}

ActorBarsService::~ActorBarsService() noexcept
{
    if (s_pLayer)
        s_pLayer->SetWanted({});
}

void ActorBarsService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    m_playerLevels[acMessage.PlayerId] = acMessage.Level;
}

void ActorBarsService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    m_playerLevels[acMessage.PlayerId] = acMessage.NewLevel;
}

void ActorBarsService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_playerLevels.erase(acMessage.PlayerId);
}

void ActorBarsService::UpdateSettings(float aDelta) noexcept
{
    if (m_settingsLoaded && (m_settingsTimer -= aDelta) > 0.f)
        return;
    m_settingsTimer = kSettingsInterval;

    std::error_code ec;
    const auto path = SettingsPath();
    const auto time = std::filesystem::last_write_time(path, ec);
    const bool there = !ec;
    if (m_settingsLoaded && (there ? time : std::filesystem::file_time_type{}) == m_settingsTime)
        return;
    m_settingsTime = there ? time : std::filesystem::file_time_type{};
    LoadSettings(there);
}

void ActorBarsService::LoadSettings(bool aFileThere) noexcept
{
    Settings settings{};
    if (aFileThere)
    {
        const IniFile ini{SettingsPath().wstring()};
        settings.Enabled = ini.Bool(L"General", L"bEnabled", settings.Enabled);
        ini.Read(L"Enemies", settings.Enemies);
        ini.Read(L"Friendly", settings.Friendly);
        settings.Players = ini.Bool(L"Friendly", L"bShowPlayers", settings.Players);
        settings.RedEnemies = ini.Bool(L"Enemies", L"bUseWarningColor", settings.RedEnemies);
        settings.FixedFriendlyColour = ini.Bool(L"Friendly", L"bUseFixedColor", settings.FixedFriendlyColour);
        settings.UppercaseNames = ini.Bool(L"Display", L"bUppercaseNames", settings.UppercaseNames);
        settings.Level = ini.Bool(L"Display", L"bShowLevel", settings.Level);
        settings.HealthBar = ini.Bool(L"Display", L"bShowHealthBar", settings.HealthBar);
        settings.ApBar = ini.Bool(L"Display", L"bShowActionPointsBar", settings.ApBar);
        settings.HideFullAp = ini.Bool(L"Display", L"bAutoHideUnusedActionPoints", settings.HideFullAp);
        settings.AnimateBars = ini.Bool(L"Display", L"bAnimateBars", settings.AnimateBars);
        settings.AnimationTime = ini.Float(L"Display", L"iBarAnimationDurationMs", settings.AnimationTime * 1000.f) / 1000.f;
        settings.HideInvisible = ini.Bool(L"Visibility", L"bHideInvisibleActors", settings.HideInvisible);
        settings.MaxDistance = ini.Float(L"Visibility", L"fMaximumDistance", settings.MaxDistance);
        settings.FadeByDistance = ini.Bool(L"Visibility", L"bFadeByDistance", settings.FadeByDistance);
        settings.FadeStart = ini.Float(L"Visibility", L"fFadeStartDistance", settings.FadeStart);
        settings.UpperHide = ini.Float(L"Visibility", L"fUpperBarHideDistance", settings.UpperHide);
        settings.ScaleByDistance = ini.Bool(L"Visibility", L"bDistanceScaling", settings.ScaleByDistance);
        settings.ScaleStart = ini.Float(L"Visibility", L"fDistanceScaleStart", settings.ScaleStart);
        settings.ScaleStrength = ini.Float(L"Visibility", L"fDistanceScaleStrength", settings.ScaleStrength);
        settings.MinScale = ini.Float(L"Visibility", L"fMinimumDistanceScale", settings.MinScale);
        settings.DeadDelay = ini.Float(L"Visibility", L"fDeadActorDelay", settings.DeadDelay);
        settings.Opacity = ini.Float(L"Visibility", L"fWidgetOpacity", settings.Opacity);
        settings.Scale = ini.Float(L"Visibility", L"fWidgetScale", settings.Scale);
        settings.MaxBars = ini.Int(L"Visibility", L"iMaximumWidgets", settings.MaxBars);
    }
    settings.AnimationTime = std::clamp(settings.AnimationTime, 0.01f, 5.f);
    settings.MaxDistance = std::clamp(settings.MaxDistance, 100.f, 20000.f);
    settings.FadeStart = std::clamp(settings.FadeStart, 0.f, settings.MaxDistance);
    settings.ScaleStart = std::max(settings.ScaleStart, 1.f);
    settings.ScaleStrength = std::clamp(settings.ScaleStrength, 0.f, 4.f);
    settings.MinScale = std::clamp(settings.MinScale, 0.05f, 1.f);
    settings.DeadDelay = std::clamp(settings.DeadDelay, 0.f, 30.f);
    settings.Opacity = std::clamp(settings.Opacity, 0.f, 1.f);
    settings.Scale = std::clamp(settings.Scale, 0.1f, 4.f);
    settings.MaxBars = std::clamp(settings.MaxBars, 1, static_cast<int>(kMaxSprites));

    m_settings = settings;
    m_settingsLoaded = true;
    // the name lines follow the new settings at the next refresh
    m_refreshTimer = 0.f;
    spdlog::info("Actor bars: settings {} (on {}, enemies {}, friendly {}, players {}, distance {:.0f}, scale {:.2f})", aFileThere ? "from MCM" : "default", settings.Enabled,
                 settings.Enemies.Show, settings.Friendly.Show, settings.Players, settings.MaxDistance, settings.Scale);
}

bool ActorBarsService::IsStandingDown() noexcept
{
    if (m_standingDown < 0)
    {
        m_standingDown = 0;
        for (const wchar_t* pDll : kActorWidgetsDlls)
        {
            if (GetModuleHandleW(pDll))
            {
                m_standingDown = 1;
                spdlog::warn(L"Actor bars: off, Actor Widgets ({}) is loaded and draws its own (remove it to get ours)", pDll);
            }
        }
    }
    return m_standingDown == 1;
}

void ActorBarsService::ResolveForms() noexcept
{
    if (m_formsResolved)
        return;
    m_formsResolved = true;
    m_pLegendaryKeyword = TESForm::GetById(kLegendaryKeyword);
    const auto skull = FindGameSettingInt(kSkullSetting);
    m_skullDifference = skull.value_or(kSkullDefault);
    spdlog::info("Actor bars: legendary keyword {}, skull at {} levels over the player{}", m_pLegendaryKeyword != nullptr, m_skullDifference,
                 skull ? "" : " (the setting wasn't found, the game's default)");
}

void ActorBarsService::Clear() noexcept
{
    m_tracked.clear();
    m_refreshTimer = 0.f;
    if (m_showing)
    {
        m_showing = false;
        s_pLayer->SetWanted({});
    }
}

void ActorBarsService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("ActorBars");
    const float delta = static_cast<float>(acEvent.Delta);
    UpdateSettings(delta);

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    NiPoint3 camera{};
    if (!m_settings.Enabled || !pPlayer || !pPlayer->parentCell || IsStandingDown() || IsHiddenByMenu() || !HudMarker::GetCameraPosition(camera))
    {
        Clear();
        return;
    }

    if ((m_refreshTimer -= delta) <= 0.f)
    {
        m_refreshTimer = kRefreshInterval;
        Refresh(pPlayer, camera);
    }
    Show(pPlayer, camera, delta);
}

bool ActorBarsService::GetHead(Actor* apActor, const Tracked& acTracked, NiPoint3& aOut) const noexcept
{
    auto* pRoot = reinterpret_cast<uint8_t*>(apActor->GetNiNode());
    if (!pRoot)
        return false;
    if (acTracked.HeadNode >= 0)
    {
        if (const uint8_t* pHead = GetObjectByName(pRoot, HeadNodeName(acTracked.HeadNode)))
        {
            const auto* pWorld = reinterpret_cast<const float*>(pHead + kWorldTranslate);
            aOut = NiPoint3(glm::vec3(pWorld[0], pWorld[1], pWorld[2]));
            return true;
        }
    }
    // without a head node: the top of the sphere round its 3D, over where it stands
    const auto* pBound = reinterpret_cast<const float*>(pRoot + kWorldBound);
    aOut = NiPoint3(glm::vec3(apActor->position.x, apActor->position.y, std::max(pBound[2] + pBound[3], apActor->position.z)));
    return true;
}

void ActorBarsService::Refresh(PlayerCharacter* apPlayer, const NiPoint3& acCamera) noexcept
{
    ResolveForms();

    // the other players' characters here (by form ID), with their player names and levels (0 while not known)
    struct OtherPlayer
    {
        std::string Name;
        uint16_t Level;
    };
    std::unordered_map<uint32_t, OtherPlayer> players;
    {
        const auto& names = m_world.GetPartyService().GetPlayers();
        auto view = m_world.view<FormIdComponent, PlayerComponent>();
        for (auto entity : view)
        {
            const uint32_t playerId = view.get<PlayerComponent>(entity).Id;
            const auto name = names.find(playerId);
            const auto level = m_playerLevels.find(playerId);
            players[view.get<FormIdComponent>(entity).Id] = {name != names.end() ? name->second.c_str() : "", level != m_playerLevels.end() ? level->second : uint16_t(0)};
        }
    }

    for (auto& [id, tracked] : m_tracked)
        tracked.Seen = false;

    const float reach = m_settings.MaxDistance + kRefreshMargin;
    const bool playerInCombat = apPlayer->IsInCombat();
    const int32_t playerLevel = apPlayer->GetLevel();
    const uint32_t filter = WorldRays::GetLineOfSightFilter(apPlayer);

    const auto visit = [&](Actor* apActor)
    {
        if (!apActor || apActor == apPlayer || apActor->IsDisabled() || !apActor->GetNiNode())
            return;
        if (glm::distance(glm::vec3(apActor->position), glm::vec3(acCamera)) > reach)
            return;
        auto existing = m_tracked.find(apActor->formID);
        if (existing != m_tracked.end() && existing->second.Seen)
            return;

        const auto player = players.find(apActor->formID);
        const bool isPlayer = player != players.end();
        std::string name = isPlayer ? player->second.Name : std::string();
        if (name.empty())
            name = NameOf(apActor);
        if (name.empty())
            return;

        Tracked& tracked = m_tracked[apActor->formID];
        tracked.Seen = true;
        tracked.Player = isPlayer;
        tracked.Dead = apActor->IsDead();
        // the dead keep the side they died on
        if (!tracked.Dead)
        {
            tracked.InCombat = apActor->IsInCombat() || (isPlayer && playerInCombat);
            bool enemy = false;
            if (!isPlayer)
            {
                enemy = IsHostileTo(apActor, apPlayer);
                if (!enemy && tracked.InCombat)
                {
                    // at war with us without being hostile to us (a settler shot at)
                    const Actor* pTarget = apActor->GetCombatTarget();
                    enemy = pTarget && (pTarget == apPlayer || players.contains(pTarget->formID));
                }
            }
            tracked.Enemy = enemy;
        }
        const Rules& rules = tracked.Enemy ? m_settings.Enemies : m_settings.Friendly;
        // another player's copy here isn't at their level: theirs comes from the server
        const int32_t level = isPlayer ? player->second.Level : apActor->GetLevel();
        tracked.Legendary = !isPlayer && m_pLegendaryKeyword && HasKeyword(apActor, m_pLegendaryKeyword);
        tracked.Skull = !isPlayer && m_skullDifference > 0 && level >= playerLevel + m_skullDifference;
        tracked.Invisible = apActor->GetActorValue(ActorValueInfo::kInvisibility) > 0.f;

        // the name line; a skull stands for the level
        std::string text = m_settings.UppercaseNames ? Uppercase(std::move(name)) : std::move(name);
        if (m_settings.Level && level > 0 && !(tracked.Skull && rules.Skull))
            text += m_settings.UppercaseNames ? fmt::format("  LVL {}", level) : fmt::format("  Lvl {}", level);
        tracked.Text = std::move(text);

        tracked.HeadNode = -1;
        auto* pRoot = reinterpret_cast<uint8_t*>(apActor->GetNiNode());
        for (int i = 0; i < static_cast<int>(std::size(kHeadNodes)); ++i)
        {
            if (GetObjectByName(pRoot, HeadNodeName(i)))
            {
                tracked.HeadNode = i;
                break;
            }
        }

        // a wall between the camera and the head; see-through things (glass, fences) and characters don't count
        tracked.Occluded = false;
        NiPoint3 head{};
        if (rules.HideBehindWalls && GetHead(apActor, tracked, head))
        {
            const glm::vec3 from(acCamera);
            const glm::vec3 to(head);
            const float length = glm::distance(from, to);
            if (length > kWallSlack)
            {
                const NiPoint3 end(from + (to - from) * ((length - kWallSlack) / length));
                const WorldRays::RayHit hit = WorldRays::PickOnce(apPlayer->parentCell, filter, acCamera, end);
                tracked.Occluded = hit.Hit && !WorldRays::IsCharacterLayer(hit.Layer);
            }
        }
    };

    if (ProcessLists* pLists = ProcessLists::Get())
    {
        for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
            visit(Cast<Actor>(TESObjectREFR::GetByHandle(pLists->highActorHandleArray[i])));
    }
    for (const auto& [formId, other] : players)
        visit(Cast<Actor>(TESForm::GetById(formId)));

    std::erase_if(m_tracked, [](const auto& acEntry) { return !acEntry.second.Seen; });
}

void ActorBarsService::UpdateTrail(Tracked& aTracked, float aHealth, float aDelta) const noexcept
{
    if (!m_settings.AnimateBars || aTracked.LastHealth < 0.f || aHealth >= aTracked.Trail)
    {
        aTracked.TrailFrom = aTracked.Trail = aHealth;
        aTracked.TrailTime = 0.f;
    }
    else
    {
        // a new hit starts from what was shown
        if (aHealth < aTracked.LastHealth - 1e-4f)
        {
            aTracked.TrailFrom = aTracked.Trail;
            aTracked.TrailTime = 0.f;
        }
        aTracked.TrailTime += aDelta;
        const float shrunk = std::clamp((aTracked.TrailTime - kTrailHold) / m_settings.AnimationTime, 0.f, 1.f);
        aTracked.Trail = std::max(aHealth, aTracked.TrailFrom + (aHealth - aTracked.TrailFrom) * shrunk);
    }
    aTracked.LastHealth = aHealth;
}

void ActorBarsService::Show(PlayerCharacter* apPlayer, const NiPoint3& acCamera, float aDelta) noexcept
{
    struct Candidate
    {
        uint32_t Id;
        Tracked* pTracked;
        NiPoint3 Head;
        float Scale;
        float Health;
        float Ap;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(m_tracked.size());

    // while aiming down the sights: the nearest one the middle of the view goes through
    const uint32_t gunState = (apPlayer->actorState.flags2 >> 14) & 0xF;
    const bool aiming = gunState == kGunSighted || gunState == kGunFireSighted;
    uint32_t aimed = 0;
    float aimedAhead = std::numeric_limits<float>::max();

    for (auto& [id, tracked] : m_tracked)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(id));
        NiPoint3 head{};
        if (!pActor || !GetHead(pActor, tracked, head))
        {
            tracked.Alpha = 0.f;
            continue;
        }
        const float maxHealth = pActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        const float health = maxHealth > 0.f ? std::clamp(pActor->GetActorValue(ActorValueInfo::kHealth) / maxHealth, 0.f, 1.f) : 0.f;
        const float maxAp = pActor->GetActorPermanentValue(ActorValueInfo::kActionPoints);
        const float ap = maxAp > 0.f ? std::clamp(pActor->GetActorValue(ActorValueInfo::kActionPoints) / maxAp, 0.f, 1.f) : -1.f;
        const float scale = pActor->GetScale();
        candidates.push_back({id, &tracked, head, scale, health, ap});

        float headRight, headUp, headAhead, feetRight, feetUp, feetAhead;
        if (aiming && !tracked.Dead && !tracked.Occluded && health > 0.f && HudMarker::WorldToView(head, headRight, headUp, headAhead) &&
            HudMarker::WorldToView(pActor->position, feetRight, feetUp, feetAhead) && headAhead > 0.f && feetAhead > 0.f)
        {
            const float right = 0.5f * (headRight + feetRight);
            if (std::abs(right) < kAimRadius * scale && feetUp <= 0.f && headUp + kAimOverHead * scale >= 0.f && headAhead < aimedAhead)
            {
                aimed = id;
                aimedAhead = headAhead;
            }
        }
    }

    std::vector<Bar> bars;
    for (const Candidate& candidate : candidates)
    {
        Tracked& tracked = *candidate.pTracked;
        const Rules& rules = tracked.Enemy ? m_settings.Enemies : m_settings.Friendly;
        UpdateTrail(tracked, candidate.Health, aDelta);

        bool wanted = rules.Show && (!tracked.Player || m_settings.Players);
        if (tracked.Dead || candidate.Health <= 0.f)
        {
            // a bar that was up when its character died stays a moment
            tracked.DeadTime += aDelta;
            wanted = wanted && tracked.Alpha > 0.f && tracked.DeadTime < m_settings.DeadDelay;
        }
        else
        {
            tracked.DeadTime = 0.f;
            if (rules.AimedTargetOnly && aiming)
                wanted = wanted && candidate.Id == aimed;
            else if (rules.CombatOnly || rules.MissingHealthOnly)
                wanted = wanted && (!rules.CombatOnly || tracked.InCombat) && (!rules.MissingHealthOnly || candidate.Health < kFullHealth);
            else
                wanted = wanted && rules.AlwaysDisplay;
            wanted = wanted && !(m_settings.HideInvisible && tracked.Invisible) && !(rules.HideBehindWalls && tracked.Occluded);
        }

        const NiPoint3 anchor(glm::vec3(candidate.Head.x, candidate.Head.y, candidate.Head.z + (tracked.HeadNode >= 0 ? kOverHead : kOverBounds) * candidate.Scale));
        const float distance = glm::distance(glm::vec3(anchor), glm::vec3(acCamera));
        wanted = wanted && distance <= m_settings.MaxDistance && distance >= kMinDistance;
        tracked.Alpha = std::clamp(tracked.Alpha + (wanted ? aDelta / kFadeIn : -aDelta / kFadeOut), 0.f, 1.f);
        if (tracked.Alpha <= 0.f)
            continue;

        float x = 0.f;
        float y = 0.f;
        if (!HudMarker::WorldToScreen(anchor, x, y) || x < -0.1f || x > 1.1f || y < -0.1f || y > 1.1f)
            continue;

        const bool upper = distance <= m_settings.UpperHide;
        Bar bar{};
        bar.Id = candidate.Id;
        if (upper)
            bar.Text = tracked.Text;
        bar.Star = upper && tracked.Legendary && rules.LegendaryStar;
        bar.Skull = upper && tracked.Skull && rules.Skull;
        bar.HealthBar = m_settings.HealthBar;
        bar.Health = candidate.Health;
        bar.Trail = tracked.Trail;
        bar.Ap = m_settings.ApBar && !(m_settings.HideFullAp && candidate.Ap >= kFullHealth) ? candidate.Ap : -1.f;
        bar.Tint = tracked.Enemy ? (m_settings.RedEnemies ? Colour::kWarning : Colour::kHud) : (m_settings.FixedFriendlyColour ? Colour::kGreen : Colour::kHud);
        bar.X = x;
        bar.Y = y;
        const float shrink = m_settings.ScaleByDistance && distance > m_settings.ScaleStart ? std::max(m_settings.MinScale, std::pow(m_settings.ScaleStart / distance, m_settings.ScaleStrength)) : 1.f;
        bar.Scale = m_settings.Scale * shrink;
        const float fade = m_settings.FadeByDistance && distance > m_settings.FadeStart && m_settings.MaxDistance > m_settings.FadeStart
                               ? std::clamp(1.f - (distance - m_settings.FadeStart) / (m_settings.MaxDistance - m_settings.FadeStart), 0.f, 1.f)
                               : 1.f;
        bar.Alpha = m_settings.Opacity * fade * tracked.Alpha;
        bar.Distance = distance;
        bars.push_back(std::move(bar));
    }

    // the nearest ones
    std::sort(bars.begin(), bars.end(), [](const Bar& acLeft, const Bar& acRight) { return acLeft.Distance < acRight.Distance; });
    if (bars.size() > static_cast<size_t>(m_settings.MaxBars))
        bars.resize(m_settings.MaxBars);

    if (!bars.empty() || m_showing)
    {
        m_showing = !bars.empty();
        s_pLayer->SetWanted(std::move(bars));
    }
}

#endif
