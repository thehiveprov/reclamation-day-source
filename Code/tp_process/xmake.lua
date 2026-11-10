
target("TPProcess")
    set_kind("binary")
    set_group("Client")

    if is_plat("windows") then
        add_ldflags("/subsystem:windows")
    end

    -- ../../build: BuildInfo.h, the version for process.rc
    add_includedirs(
        ".",
        "../../build")
    add_headerfiles("*.h")
    add_files(
        "*.cpp",
        "process.rc")
    add_deps("UiProcess")
    add_packages("tiltedcore", "cef", "hopscotch-map")