#include "NemesisData.h"

#include <algorithm>
#include <cmath>

namespace Nemesis
{
namespace
{
// Fallout4.esm locations (LCTN) the gangs hold as camps
constexpr const char* kCorvega = "Fallout4.esm:24F93";
constexpr const char* kSuperDuperMart = "Fallout4.esm:32192";
constexpr const char* kMuseumOfFreedom = "Fallout4.esm:1D807";
constexpr const char* kThicket = "Fallout4.esm:24FAE";
constexpr const char* kWaldenPond = "Fallout4.esm:24FAF";
constexpr const char* kRationStockpile = "Fallout4.esm:36010";
constexpr const char* kLibertalia = "Fallout4.esm:E3746";
constexpr const char* kDunwich = "Fallout4.esm:49B78";
constexpr const char* kHubCity = "Fallout4.esm:4D135";
constexpr const char* kRevereBeach = "Fallout4.esm:62507";
constexpr const char* kHardwareTown = "Fallout4.esm:3E0CC";
constexpr const char* kFensCamp = "Fallout4.esm:1B8C53";
constexpr const char* kBeantown = "Fallout4.esm:3A508";
constexpr const char* kBigJohns = "Fallout4.esm:5B75F";
constexpr const char* kCombatZone = "Fallout4.esm:1905B";
constexpr const char* kEastCit = "Fallout4.esm:19D3F6";
constexpr const char* kDrugDen = "Fallout4.esm:1D7FED";
constexpr const char* kQuincy = "Fallout4.esm:5A5DD";
constexpr const char* kMassPike = "Fallout4.esm:4788D";
constexpr const char* kWrvr = "Fallout4.esm:3F2E4";
constexpr const char* kGwinnett = "Fallout4.esm:48D92";
constexpr const char* kVault114 = "Fallout4.esm:42904";
constexpr const char* kSaugus = "Fallout4.esm:42193";
constexpr const char* kTrinityTower = "Fallout4.esm:1DAF7";
constexpr const char* kFortStrong = "Fallout4.esm:39D8F";
constexpr const char* kBreakheart = "Fallout4.esm:2B97F";
constexpr const char* kWestEverett = "Fallout4.esm:4A4D5";
constexpr const char* kMysticPines = "Fallout4.esm:4E011";
constexpr const char* kFortHagenArray = "Fallout4.esm:26ED8";
constexpr const char* kGlowingCrater = "Fallout4.esm:23B46A";
constexpr const char* kScrapPalace = "Fallout4.esm:4C8D1";
constexpr const char* kLaytonTowers = "Fallout4.esm:162480";

std::vector<GangDef> MakeGangs()
{
    std::vector<GangDef> gangs;

    gangs.push_back({"gunners", "the Gunners", "Gunners", kBothCampaigns, "", kCommonwealth, "", "Major",
                     {{{"CAPTAINS", "Captain", 4, 3}, {"SERGEANTS", "Sergeant", 2, 6}, {"GRUNTS", "Grunt", 1, 10}}},
                     140, 4000, -1, "gunner", 30, 0, kRaiders, 1.1f, 0.8f,
                     {{"quincy", 0.6f}, {"jamaica", 0.4f}, {"southwest", 0.3f}, {"westwilds", 0.25f}, {"southboston", 0.2f}},
                     {{"rustjaws", -30}, {"saltdogs", -30}, {"butchers", -45}, {"mutants", -80}, {"minutemen", -90}, {"dcsecurity", -50}, {"atomites", -40}},
                     {{kQuincy, "Quincy Ruins"}, {kMassPike, "Mass Pike Interchange"}, {kWrvr, "WRVR Broadcast Station"}, {kGwinnett, "Gwinnett Brewery"}},
                     {"mercenary", "deserter", "drill instructor", "armorer", "radio man", "sniper", "medic", "quartermaster"}});

    gangs.push_back({"rustjaws", "the Rust Jaws", "Rust Jaws", kBothCampaigns, "", kCommonwealth, "", "Boss",
                     {{{"LIEUTENANTS", "Lieutenant", 4, 2}, {"BRUISERS", "Bruiser", 2, 5}, {"JAWS", "Jaw", 1, 12}}},
                     90, 1200, -2, "raider", 35, 8, kRaiders, 1.2f, 1.3f,
                     {{"northwest", 0.5f}, {"lexington", 0.6f}, {"graygarden", 0.3f}},
                     {{"gunners", -30}, {"saltdogs", -10}, {"butchers", -20}, {"mutants", -70}, {"minutemen", -90}},
                     {{kCorvega, "Corvega Assembly Plant"}, {kSuperDuperMart, "Super Duper Mart"}, {kMuseumOfFreedom, "Museum of Freedom"}, {kThicket, "Thicket Excavations"},
                      {kWaldenPond, "Walden Pond"}, {kRationStockpile, "Federal Ration Stockpile"}},
                     {"scrapper", "chem cook", "brahmin thief", "knife fighter", "fence", "tire burner", "pit fighter", "lookout"}});

    gangs.push_back({"saltdogs", "the Salt Dogs", "Salt Dogs", kBothCampaigns, "", kCommonwealth, "", "Captain",
                     {{{"MATES", "Mate", 4, 2}, {"BOSUNS", "Bosun", 2, 5}, {"DOGS", "Dog", 1, 12}}},
                     90, 1500, -2, "raider", 35, 10, kRaiders, 1.2f, 1.3f,
                     {{"revere", 0.5f}, {"lynn", 0.25f}, {"northshore", 0.35f}, {"harbor", 0.3f}},
                     {{"gunners", -30}, {"rustjaws", -10}, {"butchers", -25}, {"forged", -40}, {"mutants", -70}},
                     {{kLibertalia, "Libertalia"}, {kDunwich, "Dunwich Borers"}, {kHubCity, "Hub City Auto Wreckers"}, {kRevereBeach, "Revere Beach Station"}},
                     {"boat thief", "net mender", "smuggler", "wrecker", "chem runner", "dock rat", "lookout", "fence"}});

    gangs.push_back({"butchers", "the Turnpike Butchers", "Butchers", kBothCampaigns, "", kCommonwealth, "", "Boss",
                     {{{"CLEAVERS", "Cleaver", 4, 2}, {"HOOKS", "Hook", 2, 5}, {"BUTCHERS", "Butcher", 1, 12}}},
                     100, 1400, -2, "raider", 35, 10, kRaiders, 1.2f, 1.4f,
                     {{"southwest", 0.5f}, {"fens", 0.3f}, {"southboston", 0.3f}, {"downtown", 0.15f}, {"cambridge", 0.25f}},
                     {{"gunners", -45}, {"rustjaws", -20}, {"saltdogs", -25}, {"triggermen", -35}, {"dcsecurity", -80}, {"mutants", -70}},
                     {{kHardwareTown, "Hardware Town"}, {kFensCamp, "the Fens raider camp"}, {kBeantown, "Beantown Brewery"}, {kBigJohns, "Big John's Salvage"},
                      {kCombatZone, "the Combat Zone"}, {kEastCit, "the East C.I.T. raider camp"}},
                     {"pit fighter", "chem cook", "toll taker", "scrapper", "slaver", "knife fighter", "fence", "bookie"}});

    gangs.push_back({"triggermen", "the Triggermen", "Triggermen", kBothCampaigns, "", kCommonwealth, "Skinny Malone", "Boss",
                     {{{"CAPOS", "Capo", 4, 2}, {"MADE MEN", "Made man", 2, 4}, {"TRIGGERMEN", "Triggerman", 1, 8}}},
                     45, 3000, -1, "triggerman", 10, 25, 0, 0.8f, 1.2f,
                     {{"downtown", 0.35f}, {"charlestown", 0.3f}},
                     {{"butchers", -35}, {"gunners", -20}, {"dcsecurity", -40}, {"mutants", -60}},
                     {{kVault114, "Vault 114"}, {kDrugDen, "the Charlestown drug den"}},
                     {"bootlegger", "numbers runner", "collector", "torpedo", "bookie", "driver", "fence", "card sharp"}});

    gangs.push_back({"forged", "the Forged", "Forged", kBothCampaigns, "", kCommonwealth, "", "Foreman",
                     {{{"FOREMEN", "Foreman", 4, 2}, {"BURNERS", "Burner", 2, 4}, {"FORGED", "Forged", 1, 10}}},
                     60, 900, -2, "raider", 20, 5, kRaiders, 1.0f, 1.2f,
                     {{"lynn", 0.5f}, {"malden", 0.15f}},
                     {{"saltdogs", -40}, {"mutants", -60}, {"gunners", -30}},
                     {{kSaugus, "Saugus Ironworks"}},
                     {"smith", "flamer", "branding man", "furnace tender", "slaver", "scrapper"}});

    gangs.push_back({"mutants", "the super mutant warbands", "Super Mutants", kBothCampaigns, "", kCommonwealth, "", "Warlord",
                     {{{"OVERLORDS", "Overlord", 4, 2}, {"MASTERS", "Master", 2, 5}, {"BRUTES", "Brute", 1, 12}}},
                     110, 200, -1, "mutant", 0, 0, kMutantNames, 1.0f, 1.0f,
                     {{"malden", 0.5f}, {"charlestown", 0.3f}, {"downtown", 0.2f}, {"harbor", 0.4f}, {"lexington", 0.2f}, {"cambridge", 0.2f}},
                     {{"gunners", -80}, {"rustjaws", -70}, {"saltdogs", -70}, {"butchers", -70}, {"forged", -60}, {"triggermen", -60}, {"minutemen", -90},
                      {"dcsecurity", -90}, {"atomites", -50}},
                     {{kTrinityTower, "Trinity Tower"}, {kFortStrong, "Fort Strong"}, {kBreakheart, "Breakheart Banks"}, {kWestEverett, "West Everett Estates"},
                      {kMysticPines, "Mystic Pines"}},
                     {"meat hauler", "drummer", "gatherer", "hound keeper", "skull stacker"}});

    gangs.push_back({"atom", "the Children of Atom", "Children of Atom", kBothCampaigns, "", kCommonwealth, "Mother Isolde", "Mother",
                     {{{"CONFESSORS", "Confessor", 4, 2}, {"BROTHERS AND SISTERS", "Brother", 2, 4}, {"FAITHFUL", "Faithful", 1, 10}}},
                     40, 300, 0, "atom", 45, 0, kProvokedOnly | kNoWars | kAtomNames, 0.7f, 1.0f,
                     {{"glowingsea", 0.7f}},
                     {{"atomites", -20}, {"gunners", -30}, {"mutants", -40}},
                     {},
                     {"preacher", "glow reader", "scavenger", "isotope keeper", "pilgrim guide"}});

    gangs.push_back({"atomites", "the Atomites", "Atomites", kWinterOfAtom, "", kCommonwealth, "the Last Son of Atom", "Prophet",
                     {{{"APOSTLES", "Apostle", 4, 3}, {"ZEALOTS", "Zealot", 2, 6}, {"ATOMITES", "Atomite", 1, 12}}},
                     90, 1500, -2, "atom", 45, 5, kAtomNames, 1.2f, 1.1f,
                     {{"glowingsea", 0.6f}, {"southwest", 0.2f}, {"fens", 0.1f}, {"westwilds", 0.2f}},
                     {{"atom", -20}, {"gunners", -40}, {"mutants", -50}, {"dcsecurity", -60}, {"minutemen", -70}},
                     {{kGlowingCrater, "the Glowing Sea crater"}, {kFortHagenArray, "Fort Hagen Satellite Array"}},
                     {"recruiter", "radio preacher", "isotope runner", "tunnel digger", "relic hunter", "zealot"}});

    gangs.push_back({"thrash", "the Thrash", "Thrash", kWinterOfAtom, "RD_WinterOfAtom.esp", kCommonwealth, "Thrash", "Boss",
                     {{{"BIG ONES", "Big one", 4, 1}, {"BASHERS", "Basher", 2, 2}, {"THRASH", "Thrash", 1, 6}}},
                     20, 200, -1, "mutant", 0, 0, kProvokedOnly | kNoWars | kMutantNames, 0.6f, 1.0f,
                     {{"cambridge", 0.3f}},
                     {{"atomites", -40}, {"mutants", -20}},
                     {},
                     {"garage guard", "scrap hauler", "brawler"}});

    gangs.push_back({"scavengers", "scavenger bands", "Scavengers", kWinterOfAtom, "", kCommonwealth, "", "",
                     {{{"", "", 9, 0}, {"", "", 9, 0}, {"SCAVENGERS", "Scavenger", 1, 16}}},
                     60, 600, 0, "scavenger", 40, 10, kProvokedOnly | kNoWars | kLoners, 0.6f, 0.8f,
                     {{"fens", 0.1f}, {"cambridge", 0.15f}, {"graygarden", 0.2f}, {"jamaica", 0.15f}, {"malden", 0.1f}},
                     {},
                     {},
                     {"scrapper", "tinker", "salvager", "prospector", "trapper", "trader"}});

    gangs.push_back({"dcsecurity", "Diamond City Security", "DC Security", kBothCampaigns, "", kCommonwealth, "", "Chief",
                     {{{"SERGEANTS", "Sergeant", 4, 2}, {"OFFICERS", "Officer", 2, 4}, {"GUARDS", "Guard", 1, 10}}},
                     40, 2500, 1, "security", 15, 0, kLawful | kProvokedOnly | kNoWars, 0.8f, 0.6f,
                     {{"fens", 0.6f}},
                     {{"butchers", -80}, {"gunners", -50}, {"triggermen", -40}, {"mutants", -90}, {"atomites", -60}},
                     {},
                     {"gate guard", "wall watch", "market patrol", "bat man", "turnkey"}});

    gangs.push_back({"minutemen", "the Minutemen holdouts", "Minutemen", kWinterOfAtom, "", kCommonwealth, "", "Colonel",
                     {{{"CAPTAINS", "Captain", 4, 1}, {"SERGEANTS", "Sergeant", 2, 3}, {"MINUTEMEN", "Minuteman", 1, 8}}},
                     30, 600, 1, "minutemen", 35, 0, kLawful | kProvokedOnly | kNoWars, 0.7f, 0.6f,
                     {{"quincy", 0.15f}, {"northwest", 0.15f}, {"graygarden", 0.1f}},
                     {{"gunners", -90}, {"rustjaws", -90}, {"butchers", -80}, {"mutants", -90}, {"atomites", -70}},
                     {},
                     {"farmer", "musket smith", "militia drummer", "scout", "radio man"}});

    gangs.push_back({"marshals", "the Marshals", "Marshals", kBothCampaigns, "KARMA.esm", kCommonwealth, "", "Head Marshal",
                     {{{"MARSHALS", "Marshal", 4, 2}, {"DEPUTIES", "Deputy", 2, 4}, {"POSSE", "Posse man", 1, 8}}},
                     30, 3000, 2, "", 25, 0, kHunters | kLawful | kNoWars, 1.0f, 0.8f,
                     {},
                     {{"butchers", -60}, {"rustjaws", -60}, {"saltdogs", -60}, {"blackledger", -80}},
                     {},
                     {"bounty hunter", "tracker", "sharpshooter"}});

    gangs.push_back({"blackledger", "the Black Ledger", "Black Ledger", kBothCampaigns, "KARMA.esm", kCommonwealth, "", "Paymaster",
                     {{{"COLLECTORS", "Collector", 4, 2}, {"BLADES", "Blade", 2, 4}, {"GUNS", "Gun", 1, 8}}},
                     30, 3000, -2, "", 25, 0, kHunters | kHuntsGood | kNoWars, 1.0f, 1.2f,
                     {},
                     {{"marshals", -80}, {"dcsecurity", -60}},
                     {},
                     {"bounty hunter", "tracker", "killer for hire"}});

    gangs.push_back({"rustdevils", "the Rust Devils", "Rust Devils", k2287, "DLCRobot.esm", kCommonwealth, "Ivey", "Boss",
                     {{{"MECHANICS", "Mechanic", 4, 2}, {"WRENCHES", "Wrench", 2, 4}, {"DEVILS", "Devil", 1, 10}}},
                     60, 1500, -2, "", 30, 0, kRaiders, 1.0f, 1.2f,
                     {{"westwilds", 0.35f}, {"graygarden", 0.15f}},
                     {{"gunners", -40}, {"rustjaws", -30}, {"mutants", -60}},
                     {{kFortHagenArray, "Fort Hagen Satellite Array"}},
                     {"bot builder", "scrapper", "welder", "chem cook"}});

    gangs.push_back({"trappers", "the Trappers", "Trappers", k2287, "DLCCoast.esm", kFarHarbor, "", "Hunter",
                     {{{"ELDERS", "Elder", 4, 1}, {"STALKERS", "Stalker", 2, 3}, {"TRAPPERS", "Trapper", 1, 8}}},
                     30, 100, -2, "", 20, 0, kProvokedOnly | kNoWars, 1.0f, 1.3f,
                     {{"farharbor", 0.5f}},
                     {},
                     {},
                     {"trapper", "skinner", "harpooner"}});

    gangs.push_back({"disciples", "the Disciples", "Disciples", k2287, "DLCNukaWorld.esm", kNukaWorld, "Nisha", "Leader",
                     {{{"BLADES", "Blade", 4, 2}, {"CUTTERS", "Cutter", 2, 4}, {"DISCIPLES", "Disciple", 1, 10}}},
                     50, 1500, -2, "", 40, 0, kProvokedOnly | kNoWars, 1.0f, 1.2f,
                     {{"nukaworld", 0.3f}},
                     {{"operators", -40}, {"pack", -40}},
                     {},
                     {"knife artist", "hunter", "flayer"}});

    gangs.push_back({"operators", "the Operators", "Operators", k2287, "DLCNukaWorld.esm", kNukaWorld, "Mags Black", "Boss",
                     {{{"FIXERS", "Fixer", 4, 2}, {"OPERATIVES", "Operative", 2, 4}, {"OPERATORS", "Operator", 1, 10}}},
                     50, 4000, -1, "", 40, 0, kProvokedOnly | kNoWars, 1.0f, 1.0f,
                     {{"nukaworld", 0.3f}},
                     {{"disciples", -40}, {"pack", -40}},
                     {},
                     {"cashier", "sharpshooter", "bookkeeper"}});

    gangs.push_back({"pack", "the Pack", "Pack", k2287, "DLCNukaWorld.esm", kNukaWorld, "Mason", "Alpha",
                     {{{"ALPHAS", "Alpha", 4, 2}, {"HOWLERS", "Howler", 2, 4}, {"PACK", "Pack animal", 1, 10}}},
                     50, 1000, -2, "", 35, 0, kProvokedOnly | kNoWars, 1.0f, 1.1f,
                     {{"nukaworld", 0.3f}},
                     {{"disciples", -40}, {"operators", -40}},
                     {},
                     {"beast tamer", "brawler", "face painter"}});

    return gangs;
}

const std::vector<District> s_districts = {
    {"northwest", "Sanctuary and Concord", kCommonwealth, -70000.f, 78000.f},
    {"lexington", "Lexington", kCommonwealth, -33000.f, 45000.f},
    {"westwilds", "Fort Hagen and Natick", kCommonwealth, -82000.f, -20000.f},
    {"graygarden", "Graygarden and Oberland", kCommonwealth, -45000.f, 10000.f},
    {"cambridge", "Cambridge", kCommonwealth, -18000.f, 10000.f},
    {"malden", "Malden and Medford", kCommonwealth, 8000.f, 58000.f},
    {"lynn", "Lynn and Saugus", kCommonwealth, 52000.f, 72000.f},
    {"northshore", "Salem and the North Shore", kCommonwealth, 90000.f, 72000.f},
    {"revere", "Revere and Nahant", kCommonwealth, 66000.f, 30000.f},
    {"charlestown", "Charlestown and the North End", kCommonwealth, 24000.f, 6000.f},
    {"downtown", "Downtown and Goodneighbor", kCommonwealth, 14000.f, -16000.f},
    {"fens", "Diamond City and the Fens", kCommonwealth, -10000.f, -28000.f},
    {"southwest", "Hardware Town and the Southwest", kCommonwealth, -35000.f, -45000.f},
    {"southboston", "South Boston", kCommonwealth, 32000.f, -45000.f},
    {"jamaica", "Jamaica Plain and Hyde Park", kCommonwealth, -2000.f, -68000.f},
    {"quincy", "Quincy and the South Shore", kCommonwealth, 30000.f, -82000.f},
    {"harbor", "the Harbor Islands", kCommonwealth, 70000.f, -35000.f},
    {"glowingsea", "the Glowing Sea", kCommonwealth, -90000.f, -100000.f},
    {"farharbor", "Far Harbor", kFarHarbor, 0.f, 0.f},
    {"nukaworld", "Nuka-World", kNukaWorld, 0.f, 0.f},
};

const std::vector<Cemetery> s_cemeteries = {
    {"Fallout4.esm:19D3F0", "the Cambridge graveyard", "cambridge"},
    {"Fallout4.esm:196AC0", "the North End graveyard", "charlestown"},
    {"Fallout4.esm:3757B", "Wildwood Cemetery", "lexington"},
};

const std::vector<const char*> s_male = {
    "Abe", "Ace", "Al", "Amos", "Angus", "Archie", "Arlo", "Art", "Augie", "Barney", "Bart", "Ben", "Benny", "Bert", "Billy", "Bo", "Boone", "Brick", "Buck", "Bud",
    "Buster", "Byron", "Cal", "Carl", "Carmine", "Cass", "Chet", "Clay", "Cliff", "Clint", "Cole", "Curtis", "Cy", "Dale", "Dallas", "Dan", "Dee", "Del", "Dex",
    "Dino", "Dirk", "Doc", "Don", "Dub", "Duke", "Dutch", "Earl", "Eddie", "Eli", "Elmer", "Emmett", "Ernie", "Ezra", "Felix", "Floyd", "Frank", "Gabe", "Gil",
    "Gus", "Hal", "Hank", "Harlan", "Hector", "Herb", "Homer", "Hoss", "Hugh", "Ike", "Ira", "Jack", "Jake", "Jasper", "Jed", "Jerry", "Jesse", "Jimmy", "Joe",
    "Joel", "Johnny", "Kurt", "Lars", "Lefty", "Len", "Leo", "Les", "Lou", "Luke", "Mack", "Marty", "Max", "Mel", "Mick", "Milo", "Moe", "Nate", "Ned", "Nick",
    "Nolan", "Oren", "Oscar", "Otis", "Pat", "Pete", "Phil", "Quinn", "Ralph", "Ray", "Red", "Reese", "Rex", "Rocco", "Roy", "Rudy", "Russ", "Sal", "Sam",
    "Saul", "Seth", "Sid", "Silas", "Stan", "Sully", "Ted", "Tex", "Theo", "Tom", "Tony", "Travis", "Troy", "Vern", "Vic", "Vince", "Wade", "Walt", "Wes",
    "Will", "Wyatt", "Zack", "Zeke",
};

const std::vector<const char*> s_female = {
    "Abby", "Ada", "Addie", "Agnes", "Alma", "Ann", "Annie", "Bea", "Belle", "Bess", "Betty", "Billie", "Bonnie", "Cass", "Clara", "Cora", "Daisy", "Dee",
    "Della", "Dolly", "Dora", "Dot", "Edie", "Ella", "Elsie", "Emma", "Etta", "Eve", "Faye", "Flo", "Fran", "Gert", "Ginny", "Greta", "Hattie", "Hazel",
    "Hilda", "Ida", "Ione", "Irma", "Ivy", "Jean", "Jess", "Jo", "Joan", "June", "Kat", "Kit", "Lacey", "Lena", "Lil", "Lola", "Lou", "Lucy", "Mabel", "Mae",
    "Maggie", "Marge", "Mary", "Maude", "Mavis", "Mel", "Millie", "Minnie", "Moira", "Molly", "Nan", "Nell", "Nora", "Olive", "Opal", "Pearl", "Peg", "Polly",
    "Rae", "Rita", "Rose", "Roxie", "Ruby", "Ruth", "Sadie", "Sal", "Sue", "Tess", "Trudy", "Val", "Vera", "Viv", "Wanda", "Wren", "Zelda",
};

const std::vector<const char*> s_surnames = {
    "Abbott", "Ackley", "Adair", "Ames", "Barlow", "Barrett", "Bascom", "Beals", "Becker", "Birch", "Blake", "Bolton", "Brandt", "Briggs", "Brody", "Burke",
    "Cahill", "Callan", "Carver", "Cassidy", "Chance", "Coffey", "Colby", "Conley", "Crane", "Crowe", "Cutler", "Dalton", "Darby", "Decker", "Dempsey",
    "Dillard", "Doane", "Doyle", "Drake", "Dunning", "Eames", "Egan", "Ellery", "Fallon", "Farley", "Finch", "Flagg", "Flint", "Foley", "Gage", "Garrity",
    "Gentry", "Gill", "Grady", "Graves", "Hadley", "Hale", "Harker", "Haskell", "Hatch", "Hawley", "Healey", "Hobbs", "Holt", "Hooper", "Hurley", "Jarvis",
    "Keane", "Kell", "Kilbride", "Kimball", "Lake", "Landry", "Larkin", "Lowell", "Lynch", "Maddox", "Mahoney", "Marsh", "Mercer", "Moody", "Mullin",
    "Nash", "Noonan", "Oakes", "Orcutt", "Pike", "Pratt", "Quill", "Ragland", "Reardon", "Ridley", "Roarke", "Rourke", "Sawyer", "Searle", "Shaw", "Shea",
    "Slade", "Sloane", "Stark", "Stone", "Sutter", "Swain", "Tabor", "Tate", "Thorne", "Tolliver", "Treat", "Tully", "Vance", "Varney", "Wade", "Walsh",
    "Ware", "Webb", "Whalen", "Wick", "Wilder", "Winslow", "Wolcott", "Yates", "Yost",
};

const std::vector<const char*> s_mutants = {
    "Grub", "Krag", "Brakk", "Thud", "Gorm", "Skarn", "Mauler", "Hulk", "Grog", "Drak", "Lunk", "Bonk", "Gash", "Rakk", "Vorg", "Thrak", "Murg", "Krull",
    "Grunk", "Snag", "Dorg", "Brute", "Smash", "Klob", "Ruk", "Torg", "Grath", "Hork", "Mug", "Zug", "Knuckle", "Skull", "Gristle", "Rend", "Crag", "Bolder",
};

const std::vector<const char*> s_atom = {
    "Brother Ash", "Brother Cobalt", "Brother Ember", "Brother Flux", "Brother Halcyon", "Brother Isotope", "Brother Kindle", "Brother Lumen", "Brother Morrow",
    "Brother Pell", "Brother Quill", "Brother Radon", "Brother Strontium", "Brother Thorium", "Brother Vigil", "Brother Wick", "Brother Zenith", "Brother Cinder",
    "Sister Aura", "Sister Beryl", "Sister Calla", "Sister Dawnlight", "Sister Ember", "Sister Gleam", "Sister Hollis", "Sister Iris", "Sister Lumina",
    "Sister Mercy", "Sister Nova", "Sister Opaline", "Sister Radia", "Sister Solace", "Sister Tamsin", "Sister Vesper", "Sister Wren", "Sister Yttria",
};
} // namespace

const std::vector<GangDef>& Gangs() noexcept
{
    static const std::vector<GangDef> s_gangs = MakeGangs();
    return s_gangs;
}

const GangDef* FindGang(const std::string& acId) noexcept
{
    for (const GangDef& gang : Gangs())
        if (acId == gang.Id)
            return &gang;
    return nullptr;
}

bool InCampaign(const GangDef& acGang, const std::string& acCampaign) noexcept
{
    const uint8_t mask = acCampaign == "winter-of-atom" ? kWinterOfAtom : k2287;
    return (acGang.Campaigns & mask) != 0;
}

std::pair<int, int> TierRange(const GangDef& acGang, size_t aTier) noexcept
{
    const int lo = acGang.Tiers[aTier].MinRank;
    const int hi = aTier == 0 ? 4 : acGang.Tiers[aTier - 1].MinRank - 1;
    return {lo, std::max(lo, hi)};
}

const std::vector<District>& Districts() noexcept
{
    return s_districts;
}

const District* FindDistrict(const std::string& acKey) noexcept
{
    for (const District& district : s_districts)
        if (acKey == district.Key)
            return &district;
    return nullptr;
}

const District* DistrictAt(const std::string& acWorld, float aX, float aY) noexcept
{
    const District* pBest = nullptr;
    float best = 0.f;
    for (const District& district : s_districts)
    {
        if (acWorld != district.World)
            continue;
        const float dx = district.X - aX;
        const float dy = district.Y - aY;
        const float distance = dx * dx + dy * dy;
        if (!pBest || distance < best)
        {
            pBest = &district;
            best = distance;
        }
    }
    return pBest;
}

const std::vector<Cemetery>& Cemeteries() noexcept
{
    return s_cemeteries;
}

const std::vector<const char*>& MaleNames() noexcept
{
    return s_male;
}

const std::vector<const char*>& FemaleNames() noexcept
{
    return s_female;
}

const std::vector<const char*>& Surnames() noexcept
{
    return s_surnames;
}

const std::vector<const char*>& MutantNames() noexcept
{
    return s_mutants;
}

const std::vector<const char*>& AtomNames() noexcept
{
    return s_atom;
}
} // namespace Nemesis
