
target("ImmersiveElf")
    set_basename("EarlyLoad")
    set_kind("shared")
    set_group("Client")
    -- ../../build: BuildInfo.h, the version for early_load.rc
    add_includedirs(
        ".",
        "../../Libraries/",
        "../../build")
    add_headerfiles("**.h")
    add_files(
        "**.cpp")
    if is_plat("windows") then
        add_files("early_load.rc")
    end