#include <catch2/catch.hpp>

#include <PluginsTxt.h>

// The launcher's load order changes to plugins.txt (Updater: InstallOurPlugins), plain text in and out
TEST_CASE("Plugins that must load last are moved to the end of plugins.txt", "[plugins]")
{
    const std::vector<std::string> last{"PRP.esp"};

    SECTION("A plugin after PRP.esp: PRP.esp goes after it, its '*' kept")
    {
        std::vector<std::string> moved;
        const std::string text = PluginsTxt::MoveToEnd("# Vortex\r\n*Unofficial.esp\r\n*PRP.esp\r\n*RD_Trees.esp\r\n", last, moved);
        REQUIRE(text == "# Vortex\r\n*Unofficial.esp\r\n*RD_Trees.esp\r\n*PRP.esp\r\n");
        REQUIRE(moved == std::vector<std::string>{"PRP.esp"});
    }

    SECTION("Already last (any case, blank lines after it): nothing changes")
    {
        std::vector<std::string> moved;
        const std::string original = "*A.esp\r\n*prp.ESP\r\n\r\n";
        REQUIRE(PluginsTxt::MoveToEnd(original, last, moved) == original);
        REQUIRE(moved.empty());
    }

    SECTION("Not listed: nothing changes")
    {
        std::vector<std::string> moved;
        const std::string original = "*A.esp\r\n*B.esp\r\n";
        REQUIRE(PluginsTxt::MoveToEnd(original, last, moved) == original);
        REQUIRE(moved.empty());
    }

    SECTION("Switched off stays off; a last line without its newline and LF-only lines keep working")
    {
        std::vector<std::string> moved;
        const std::string text = PluginsTxt::MoveToEnd("PRP.esp\n*A.esp\n*B.esp", last, moved);
        REQUIRE(text == "*A.esp\n*B.esp\nPRP.esp\r\n");
        REQUIRE(moved.size() == 1);
    }

    SECTION("Several, in the given order")
    {
        std::vector<std::string> moved;
        const std::string text = PluginsTxt::MoveToEnd("*Z.esp\r\n*A.esp\r\n*Y.esp\r\n", {"Y.esp", "Z.esp"}, moved);
        REQUIRE(text == "*A.esp\r\n*Y.esp\r\n*Z.esp\r\n");
        REQUIRE(moved == std::vector<std::string>{"Y.esp", "Z.esp"});
    }
}

TEST_CASE("Plugins are switched on and off in place", "[plugins]")
{
    std::vector<std::string> changed;
    const std::string text = PluginsTxt::SetActive("*A.esp\r\nB.esp\r\n", {"B.esp", "C.esp"}, true, changed);
    REQUIRE(text == "*A.esp\r\n*B.esp\r\n*C.esp\r\n");
    REQUIRE(changed == std::vector<std::string>{"B.esp", "C.esp"});
}
