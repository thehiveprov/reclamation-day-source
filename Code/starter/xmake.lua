-- "Launch Reclamation Day.exe" in the beta package (Tools\PackageFalloutBeta.ps1): starts Reclamation Day\ReclamationDay.exe
target("ReclamationDayStarter")
    set_kind("binary")
    set_group("Client")
    add_ldflags("/subsystem:windows", { force = true })
    add_includedirs("../../build")
    add_files(
        "main.cpp",
        "starter.rc")
    add_syslinks("user32")
