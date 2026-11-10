# Packages Fallout 4: Reclamation Day for beta testers: one zip with
#   Launch Reclamation Day.exe (the ReclamationDayStarter target), README.txt
#   Reclamation Day\  the launcher, server and overlay (extracted anywhere outside the game)
#   Game Files\       our game data (the launcher puts it in Data or in the player's mod manager); the mods by other
#                     authors it needs are downloads (README.txt)
# Built files come from build\windows\x64\release (build the launcher, server runner, TPProcess and starter first, and the UI
# with "pnpm deploy:develop"); the overlay's runtime (CEF) from distrib\bin.
param(
    [string]$OutDir = 'F:\ReclamationDay Beta',
    # The zip's (and its top folder's) name; ReclamationDay-beta-<version> by default
    [string]$Name,
    # Ours, built in their own folders: the motorcycles (plugin, meshes, settings; the F4SE plugin from its build) and
    # the dogs (plugin, and their looks under Data)
    [string]$VehiclesFolder = 'F:\modfiles\RDVehicles\dist',
    [string]$VehiclesDll = 'F:\modfiles\RDVehicles\build\windows\x64\releasedbg\RDVehicles.dll',
    [string]$CompanionsFolder = 'F:\modfiles\RDCompanions\dist',
    # The nature overhaul remade from Boston Natural Surroundings, the one mod bundled into Reclamation Day (RDNature,
    # built in F:\modfiles\RDNature; the launcher keeps it in Data itself), in Data layout (see the folder's README.txt)
    [string]$BundledFolder = 'F:\modfiles\bundled',
    # For Nexus Mods: no updater.cfg, so the package never checks for or downloads updates (Nexus doesn't allow
    # programs that update themselves; Nexus players update from the mod page)
    [switch]$NoUpdater,
    # The host's plugins (what joining checks against)
    [string[]]$LoadOrder = @(
        'Fallout4.esm', 'DLCRobot.esm', 'DLCworkshop01.esm', 'DLCCoast.esm', 'DLCworkshop02.esm', 'DLCworkshop03.esm', 'DLCNukaWorld.esm',
        'ccBGSFO4044-HellfirePowerArmor.esl', 'ccBGSFO4115-X02.esl', 'ccBGSFO4116-HeavyFlamer.esl', 'ccBGSFO4110-WS_Enclave.esl',
        'ccBGSFO4096-AS_Enclave.esl', 'ccFSVFO4007-Halloween.esl', 'ccBGSFO4046-TesCan.esl', 'ccSBJFO4003-Grenade.esl', 'ccOTMFO4001-Remnants.esl',
        'unofficial fallout 4 patch.esp', 'StartMeUpShaunDefaultAppearance.esp', 'StartMeUp.esp',
        # the graphics (required downloads), NAC X last
        'Flutter Flicker Fixer For Foliage.esp', 'Stiff Splines.esp', 'VividFallout - AiO - BestChoice.esp',
        'NAC.esp', 'NAC-FH.esp', 'NAC-NW.esp',
        # ours (in Game Files), last; RD_Combat.esp replaces Adaptive Combat Tactics' two plugins
        'RDVehicles.esp', 'RD_Companions.esp', 'RD_Combat.esp', 'RD_Perks.esp', 'RD_Camps.esp', 'RD_Nemeses.esp', 'RD_GrapplingGun.esp',
        'RD_DualWield.esp', 'RD_Social.esp',
        # KARMA, Be Exceptional and Extended Dialogue Interface (required downloads) after ours, then the nature overhaul's
        # two (bundled, switched on by the launcher; off in Winter of Atom: Game Files\ReclamationDay\CommonwealthPlugins.txt)
        'KARMA.esm', 'Be Exceptional.esp', 'XDI.esm', 'RD_Landscape.esp', 'RD_Trees.esp',
        # the Previsibines Repair Pack (a required download): PRP.esp last of all, the launcher keeps it there
        'PPF.esm', 'PRP.esp')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo 'build\windows\x64\release'
$distrib = Join-Path $repo 'distrib\bin'
$gameFiles = Join-Path $repo 'GameFiles\Fallout4'
$ui = Join-Path $repo 'Code\skyrim_ui\dist\UI'

# The version the client and server were built with (BuildInfo.h, from git describe at configure time: the tag and the
# commits since it, "v1.6.7-173"); joining needs both to match exactly. Only as a whole string, ended by a space or the
# string's end: the compiler also puts its first 8 characters in the code, to compare against.
function Get-BuildVersion([string]$File) {
    $text = [IO.File]::ReadAllText((Join-Path $build $File), [Text.Encoding]::ASCII)
    $found = @([regex]::Matches($text, 'v\d+\.\d+\.\d+-\d+(?=[\x00 ])') | ForEach-Object Value | Sort-Object -Unique)
    if ($found.Count -ne 1) { throw "can't tell which version $File was built as ($($found -join ', '))" }
    $found[0]
}
$version = Get-BuildVersion 'ReclamationDay.exe'
foreach ($file in 'ReclamationDayServer.exe', 'RDServer.dll') {
    $other = Get-BuildVersion $file
    if ($other -ne $version) { throw "$file was built as $other but ReclamationDay.exe as ${version}: rebuild them together" }
}
$head = (git -C $repo describe --tags --long).Trim() -replace '-g[0-9a-f]+$', ''
if ($head -ne $version) { Write-Warning "packaging the $version build, the source is at $head now" }

if (-not $Name) { $Name = "ReclamationDay-beta-$version" }
$stage = Join-Path $OutDir $Name
$zip = Join-Path $OutDir "$Name.zip"
if (Test-Path $stage) { throw "$stage exists already" }
if (Test-Path $zip) { throw "$zip exists already" }

function Copy-Into([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Force $Destination | Out-Null
    Copy-Item $Source $Destination -Recurse -Force
}

function Write-Text([string]$Path, [string]$Text) {
    # Windows line endings, no byte order mark
    [IO.File]::WriteAllText($Path, ($Text -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
}

# Reclamation Day: our builds, the overlay's runtime, the UI
$app = Join-Path $stage 'Reclamation Day'
foreach ($file in 'ReclamationDay.exe', 'ReclamationDayServer.exe', 'RDServer.dll', 'TPProcess.exe', 'EarlyLoad.dll') {
    Copy-Into (Join-Path $build $file) $app
}
# Discord's library, for the Discord status
Copy-Into (Join-Path $distrib 'discord_game_sdk.dll') $app
foreach ($file in 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'chrome_elf.dll', 'd3dcompiler_47.dll', 'icudtl.dat', 'libcef.dll',
    'libEGL.dll', 'libGLESv2.dll', 'resources.pak', 'snapshot_blob.bin', 'steam_appid.txt', 'v8_context_snapshot.bin', 'vk_swiftshader.dll', 'vulkan-1.dll',
    'locales', 'swiftshader', 'assets') {
    Copy-Into (Join-Path $distrib $file) $app
}
Copy-Into $ui $app
# The launcher's window (Code\launcher_ui: its page, fonts and logo, and the patch notes it came with)
Copy-Item (Join-Path $repo 'Code\launcher_ui') (Join-Path $app 'launcher') -Recurse

# Where the launcher looks for updates (see PublishUpdate.ps1)
$tokenFile = Join-Path $PSScriptRoot 'release-token.txt'
if ($NoUpdater) {
    "no updater.cfg (-NoUpdater): this package won't check for updates"
} elseif (Test-Path $tokenFile) {
    Write-Text (Join-Path $app 'updater.cfg') "repo=thehiveprov/reclamation-day-updates`ntoken=$((Get-Content $tokenFile -Raw).Trim())`n"
} else {
    Write-Warning "no $tokenFile`: this package won't be able to update itself"
}

# Game Files: ours, and the nature overhaul
$data = Join-Path $stage 'Game Files'
Copy-Into (Join-Path $gameFiles 'meshes') $data
Copy-Into (Join-Path $gameFiles 'Materials') $data
Copy-Into (Join-Path $gameFiles 'Textures') $data
Copy-Into (Join-Path $gameFiles 'sound') $data
Copy-Into (Join-Path $gameFiles 'Interface') $data
Copy-Into (Join-Path $gameFiles 'MCM') $data
Copy-Into (Join-Path $gameFiles 'CREDITS.txt') $data
Copy-Into (Join-Path $gameFiles 'RD_Combat.esp') $data
Copy-Into (Join-Path $gameFiles 'RD_Perks.esp') $data
Copy-Into (Join-Path $gameFiles 'RD_Camps.esp') $data
Copy-Into (Join-Path $gameFiles 'RD_Nemeses.esp') $data
Copy-Into (Join-Path $gameFiles 'RD_GrapplingGun.esp') $data
Copy-Into (Join-Path $gameFiles 'RD_DualWield.esp') $data
Copy-Into (Join-Path $gameFiles 'RD_Social.esp') $data
Copy-Into (Join-Path $gameFiles 'PBT') $data
# Our Papyrus scripts (namespace ReclamationDay: the launcher keeps them in Data as our other ReclamationDay folders)
Copy-Into (Join-Path $gameFiles 'Scripts') $data
# The nature overhaul and the list of its files the launcher keeps in Data (Tools/ListBundledMods.js)
Copy-Into (Join-Path $gameFiles 'ReclamationDay') $data
Get-ChildItem (Join-Path $BundledFolder 'RDNature') | ForEach-Object { Copy-Into $_.FullName $data }
Copy-Into (Join-Path $VehiclesFolder 'RDVehicles.esp') $data
Copy-Into (Join-Path $VehiclesFolder 'Meshes') $data
Copy-Into (Join-Path $VehiclesFolder 'Sound') $data
Copy-Into (Join-Path $VehiclesFolder 'Materials') $data
Copy-Into (Join-Path $VehiclesFolder 'Textures') $data
Copy-Into $VehiclesDll (Join-Path $data 'F4SE\Plugins')
Copy-Into (Join-Path $VehiclesFolder 'RDVehicles.ini') (Join-Path $data 'F4SE\Plugins')
Copy-Into (Join-Path $VehiclesFolder 'RDVehicles.routes') (Join-Path $data 'F4SE\Plugins')
Copy-Into (Join-Path $CompanionsFolder 'RD_Companions.esp') $data
Get-ChildItem (Join-Path $CompanionsFolder 'Data') | ForEach-Object { Copy-Into $_.FullName $data }

# The root: how to start it, what to do
$readme = (Get-Content (Join-Path $gameFiles 'README.txt') -Raw).Replace('__VERSION__', $version).Replace('__LOAD_ORDER__', (($LoadOrder | ForEach-Object { "  $_" }) -join "`n"))
Write-Text (Join-Path $stage 'README.txt') $readme
Copy-Item (Join-Path $build 'ReclamationDayStarter.exe') (Join-Path $stage 'Launch Reclamation Day.exe')

& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $OutDir $Name
if ($LASTEXITCODE -ne 0) { throw "zipping failed ($LASTEXITCODE)" }

$files = Get-ChildItem $stage -Recurse -File
"{0}: {1} files, {2:N0} MB unpacked, {3:N0} MB zipped" -f $zip, $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), ((Get-Item $zip).Length / 1MB)
