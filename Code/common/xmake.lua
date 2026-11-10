
target("CommonLib")
    add_configfiles("BuildInfo.h.in")
    set_kind("static")
    set_group("common")
    add_includedirs(".", "../", "../../build", {public = true})
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_packages("tiltedcore", "hopscotch-map")
    -- BUILD_COMMIT in BuildInfo.h is the version updates are named by: the tag and the commits since it, "v1.6.7-173".
    -- xmake's own GIT_TAG (git describe) ends with the commit's hash, so it's set here without it.
    -- The same version as numbers (1, 6, 7, 173) for the version resource of our programs (*.rc): 0s when git can't say.
    on_load(function (target)
        import("lib.detect.find_tool")
        local git = find_tool("git")
        local described = git and try { function () return os.iorunv(git.program, {"describe", "--tags", "--long"}) end }
        local major, minor, patch, build = "0", "0", "0", "0"
        if described then
            local tag = described:trim():gsub("%-g%x+$", "")
            target:set("configvar", "GIT_TAG", tag)
            local a, b, c, d = tag:match("^v(%d+)%.(%d+)%.(%d+)%-(%d+)$")
            if a then
                major, minor, patch, build = a, b, c, d
            end
        end
        target:set("configvar", "RD_VERSION_MAJOR", major)
        target:set("configvar", "RD_VERSION_MINOR", minor)
        target:set("configvar", "RD_VERSION_PATCH", patch)
        target:set("configvar", "RD_VERSION_BUILD", build)
    end)
