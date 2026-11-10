
target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_defines("TP_SKYRIM=1")
    add_includedirs(
        ".", "../encoding", "../server",
        -- the launcher's plugins.txt changes are plain text (see plugins.cpp)
        "../client/Games/Fallout4/Interface",
        -- the game's archive blocks decompressed whole (see inflate.cpp)
        "../client/Games/Fallout4/Loading")
    add_headerfiles("**.h")
    add_files("*.cpp")
    -- the server's nemesis engine is plain C++ (see nemesis.cpp)
    add_files("../server/Nemesis/*.cpp")
    -- and who wins a co-op dialogue vote (see dialogue.cpp)
    add_files("../server/Dialogue/*.cpp")
    add_deps("SkyrimEncoding")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "catch2",
        "mimalloc",
        "glm",
        "libdeflate",
        "zlib")
