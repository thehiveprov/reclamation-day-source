# Publishes the current build as the latest update of Fallout 4: Reclamation Day: a release of the private releases repo
# that every launcher checks at start (Code\client\Games\Fallout4\Interface\Updater.cpp). Players whose version differs
# get "Update and restart" in the main menu.
#
# A release has manifest.txt (each file of ours: SHA-256, size, pack, path in the install) and the packs: app.zip (the
# exes, server, starter, updater.cfg), ui.zip (the overlay), runtime.zip (its browser runtime), gamefiles.zip (our game
# files). Launchers only download the packs holding files they don't have.
#
# Only our files: the mods we need (UFO4P, Start Me Up, AVF, Garden of Eden, F4SE...) come from their authors, the main
# menu sends players to them.
#
# Needs Tools\release-token.txt: the read-only key launchers use to download (a fine-grained GitHub token with read
# access to the contents of the releases repo only). It goes into every install's updater.cfg, so it must never have more
# access than that. Publishing itself uses your own GitHub sign-in (Git Credential Manager).
#
# Build first (clean configure, then the launcher, server runner, TPProcess and starter, and the UI with
# "pnpm deploy:develop"), commit, then publish.
param(
    [string]$Repo = 'thehiveprov/reclamation-day-updates',
    # Shown on the release (not to players yet)
    [string]$Notes = '',
    # Don't ask before publishing
    [switch]$Yes,
    # Make the packs and manifest but don't upload them
    [switch]$DryRun,
    # Ours, built in their own folders (as PackageFalloutBeta.ps1): the motorcycles and the dogs
    [string]$VehiclesFolder = 'F:\modfiles\RDVehicles\dist',
    [string]$VehiclesDll = 'F:\modfiles\RDVehicles\build\windows\x64\releasedbg\RDVehicles.dll',
    [string]$CompanionsFolder = 'F:\modfiles\RDCompanions\dist',
    # The nature overhaul remade from Boston Natural Surroundings, the one mod bundled into Reclamation Day (RDNature, as
    # PackageFalloutBeta.ps1)
    [string]$BundledFolder = 'F:\modfiles\bundled'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot
$build = Join-Path $repoRoot 'build\windows\x64\release'
$distrib = Join-Path $repoRoot 'distrib\bin'
$gameFiles = Join-Path $repoRoot 'GameFiles\Fallout4'
$ui = Join-Path $repoRoot 'Code\skyrim_ui\dist\UI'
$tokenFile = Join-Path $PSScriptRoot 'release-token.txt'

# The version the client and server were built with; it's the release's name, and launchers compare it to their own.
# Versions are the tag and the commits since it, "v1.6.7-173" (Tools\modules\version.lua). Only as a whole string, ended
# by a space or the string's end: the compiler also puts its first 8 characters in the code, to compare against.
function Get-BuildVersion([string]$File) {
    $text = [IO.File]::ReadAllText((Join-Path $build $File), [Text.Encoding]::ASCII)
    $found = @([regex]::Matches($text, 'v\d+\.\d+\.\d+-\d+(?=[\x00 ])') | ForEach-Object Value | Sort-Object -Unique)
    if ($found.Count -ne 1) { throw "can't tell which version $File was built as ($($found -join ', '))" }
    $found[0]
}
function Get-SourceVersion([string]$Revision = 'HEAD') { (git -C $repoRoot describe --tags --long $Revision).Trim() -replace '-g[0-9a-f]+$', '' }
$version = Get-BuildVersion 'ReclamationDay.exe'
foreach ($file in 'ReclamationDayServer.exe', 'RDServer.dll') {
    $other = Get-BuildVersion $file
    if ($other -ne $version) { throw "$file was built as $other but ReclamationDay.exe as ${version}: rebuild them together" }
}
$head = Get-SourceVersion
$problems = @()
# The commit the build was made from: as many commits back from HEAD as the source is ahead of it
$behind = [int]($head -replace '^.*-', '') - [int]($version -replace '^.*-', '')
$buildCommit = "HEAD~$behind"
if ($behind -lt 0 -or (Get-SourceVersion $buildCommit) -ne $version) {
    $problems += "can't find the commit the $version build was made from (the source is at ${head}): configure and build again first"
} else {
    # The game's source since the build (other commits, e.g. to these tools, don't matter)
    git -C $repoRoot diff --quiet $buildCommit HEAD -- Code GameFiles
    if ($LASTEXITCODE -ne 0) { $problems += "the build is $version but Code or GameFiles changed since (the source is at ${head}): configure and build again first" }
}
if ((git -C $repoRoot status --porcelain --untracked-files=no -- Code GameFiles) -ne $null) { $problems += "Code or GameFiles has uncommitted changes: commit them first" }
foreach ($folder in $VehiclesFolder, $CompanionsFolder) {
    $source = Split-Path $folder
    # (RDVehicles' CommonLibF4 carries a patch of its own, see its xmake.lua)
    if ((git -C $source status --porcelain --untracked-files=no --ignore-submodules=dirty) -ne $null) { $problems += "$source has uncommitted changes: commit them first" }
}
if (-not (Test-Path $tokenFile)) { $problems += "no $tokenFile`: put the launchers' read-only key in it (see the top of this script)" }
if ($problems) {
    if (-not $DryRun) { throw ($problems -join "`n") }
    $problems | ForEach-Object { Write-Warning $_ }
}
$token = if (Test-Path $tokenFile) { (Get-Content $tokenFile -Raw).Trim() } else { "dry-run" }

$stage = Join-Path $env:TEMP "ReclamationDayPublish\$version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$files = New-Object System.Collections.Generic.List[object]

# Adds a file to a pack under its path in the install
function Add-File([string]$Source, [string]$Path, [string]$Pack) {
    $target = Join-Path (Join-Path $stage $Pack) $Path
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item $Source $target
    $files.Add([pscustomobject]@{ Path = $Path; Pack = $Pack; Target = $target })
}

# Adds a folder's files, under a folder of the install
function Add-Folder([string]$Source, [string]$Path, [string]$Pack) {
    $sourceRoot = (Resolve-Path $Source).Path
    Get-ChildItem $sourceRoot -Recurse -File | ForEach-Object {
        Add-File $_.FullName (Join-Path $Path $_.FullName.Substring($sourceRoot.Length + 1)) $Pack
    }
}

$app = 'Reclamation Day'
Add-File (Join-Path $build 'ReclamationDayStarter.exe') 'Launch Reclamation Day.exe' 'app'
foreach ($file in 'ReclamationDay.exe', 'ReclamationDayServer.exe', 'RDServer.dll', 'TPProcess.exe', 'EarlyLoad.dll') {
    Add-File (Join-Path $build $file) (Join-Path $app $file) 'app'
}
# Discord's library, for the Discord status (the Game SDK the client is built against; from the build's discord package,
# copied into distribin). In the small pack, so adding it doesn't make everyone download the runtime again.
Add-File (Join-Path $distrib 'discord_game_sdk.dll') (Join-Path $app 'discord_game_sdk.dll') 'app'
$config = Join-Path $env:TEMP 'updater.cfg'
[IO.File]::WriteAllText($config, "repo=$Repo`r`ntoken=$token`r`n", (New-Object Text.UTF8Encoding($false)))
Add-File $config (Join-Path $app 'updater.cfg') 'app'
Remove-Item $config

Add-Folder $ui (Join-Path $app 'UI') 'ui'
# The launcher's window (Code\launcher_ui: its page, fonts and logo, and the patch notes it came with)
Add-Folder (Join-Path $repoRoot 'Code\launcher_ui') (Join-Path $app 'launcher') 'ui'

foreach ($file in 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'chrome_elf.dll', 'd3dcompiler_47.dll', 'icudtl.dat', 'libcef.dll',
    'libEGL.dll', 'libGLESv2.dll', 'resources.pak', 'snapshot_blob.bin', 'steam_appid.txt', 'v8_context_snapshot.bin', 'vk_swiftshader.dll', 'vulkan-1.dll') {
    Add-File (Join-Path $distrib $file) (Join-Path $app $file) 'runtime'
}
foreach ($folder in 'locales', 'swiftshader', 'assets') {
    Add-Folder (Join-Path $distrib $folder) (Join-Path $app $folder) 'runtime'
}

Add-Folder (Join-Path $gameFiles 'meshes') 'Game Files\meshes' 'gamefiles'
Add-Folder (Join-Path $gameFiles 'sound') 'Game Files\sound' 'gamefiles'
Add-Folder (Join-Path $gameFiles 'Interface') 'Game Files\Interface' 'gamefiles'
# Our MCM page (MCM\Config\ReclamationDay: the launcher copies it into Data like our other ReclamationDay folders)
Add-Folder (Join-Path $gameFiles 'MCM') 'Game Files\MCM' 'gamefiles'
Add-File (Join-Path $gameFiles 'CREDITS.txt') 'Game Files\CREDITS.txt' 'gamefiles'
# The combat AI plugin (Tools/BuildCombatPlugin.js), also put in Data by the launcher
Add-File (Join-Path $gameFiles 'RD_Combat.esp') 'Game Files\RD_Combat.esp' 'gamefiles'
# The perks (Tools/BuildPerksPlugin.js), also put in Data by the launcher
Add-File (Join-Path $gameFiles 'RD_Perks.esp') 'Game Files\RD_Perks.esp' 'gamefiles'
# C.A.M.P.s and nemeses (Tools/BuildCampsPlugin.js, Tools/BuildNemesesPlugin.js), also put in Data by the launcher
Add-File (Join-Path $gameFiles 'RD_Camps.esp') 'Game Files\RD_Camps.esp' 'gamefiles'
Add-File (Join-Path $gameFiles 'RD_Nemeses.esp') 'Game Files\RD_Nemeses.esp' 'gamefiles'
# The grappling gun (Tools/BuildGrapplePlugin.js; its meshes, material and textures are in the folders below)
Add-File (Join-Path $gameFiles 'RD_GrapplingGun.esp') 'Game Files\RD_GrapplingGun.esp' 'gamefiles'
# Dual wielding's globals: the left hand's weapon kept in the save (Tools/BuildDualWieldPlugin.js)
Add-File (Join-Path $gameFiles 'RD_DualWield.esp') 'Game Files\RD_DualWield.esp' 'gamefiles'
# The Pip-Boy's SOCIAL tab, Steam friends (Tools/BuildSocialPlugin.js; its PBT settings and script are in the folders below)
Add-File (Join-Path $gameFiles 'RD_Social.esp') 'Game Files\RD_Social.esp' 'gamefiles'
# The NEMESIS and SOCIAL tabs' settings (PipboyTabs reads Data\PBT; the launcher keeps them there, Updater's kOurDataFiles)
Add-Folder (Join-Path $gameFiles 'PBT') 'Game Files\PBT' 'gamefiles'
# Our Papyrus scripts and the C.A.M.P. device's materials and textures (ReclamationDay folders, kept in Data too)
Add-Folder (Join-Path $gameFiles 'Scripts') 'Game Files\Scripts' 'gamefiles'
Add-Folder (Join-Path $gameFiles 'Materials') 'Game Files\Materials' 'gamefiles'
Add-Folder (Join-Path $gameFiles 'Textures') 'Game Files\Textures' 'gamefiles'

# The motorcycles and the dogs. Their plugins, at the top of Game Files, the launcher puts in Data itself (see
# Updater::InstallOurPlugins). The dogs' looks are a pack of their own, so later updates don't download them again.
Add-File (Join-Path $VehiclesFolder 'RDVehicles.esp') 'Game Files\RDVehicles.esp' 'gamefiles'
Add-Folder (Join-Path $VehiclesFolder 'Meshes') 'Game Files\Meshes' 'gamefiles'
Add-Folder (Join-Path $VehiclesFolder 'Sound') 'Game Files\Sound' 'gamefiles'
Add-Folder (Join-Path $VehiclesFolder 'Materials') 'Game Files\Materials' 'gamefiles'
Add-Folder (Join-Path $VehiclesFolder 'Textures') 'Game Files\Textures' 'gamefiles'
Add-File $VehiclesDll 'Game Files\F4SE\Plugins\RDVehicles.dll' 'gamefiles'
Add-File (Join-Path $VehiclesFolder 'RDVehicles.ini') 'Game Files\F4SE\Plugins\RDVehicles.ini' 'gamefiles'
Add-File (Join-Path $VehiclesFolder 'RDVehicles.routes') 'Game Files\F4SE\Plugins\RDVehicles.routes' 'gamefiles'
Add-File (Join-Path $CompanionsFolder 'RD_Companions.esp') 'Game Files\RD_Companions.esp' 'gamefiles'
Add-Folder (Join-Path $CompanionsFolder 'Data') 'Game Files' 'dogs'

# The list of the nature overhaul's files the launcher keeps in Data itself (Tools/ListBundledMods.js, Updater's
# InstallBundledMods). KARMA, Be Exceptional, PipboyTabs, LevelUpMenuEx and Extended Dialogue Interface were bundled up
# to 1.6.7-285: they're downloads now (the launcher's window and the main menu list them), and players who had them keep their copies in Data.
Add-File (Join-Path $gameFiles 'ReclamationDay\BundledMods.txt') 'Game Files\ReclamationDay\BundledMods.txt' 'gamefiles'
# The nature overhaul (RD_Landscape.esp, RD_Trees.esp and their archives, about 870 MB): a pack of its own, so a change
# to anything else doesn't download it again. The list of the plugins Winter of Atom switches off goes with ours.
Add-Folder (Join-Path $BundledFolder 'RDNature') 'Game Files' 'nature'
Add-File (Join-Path $gameFiles 'ReclamationDay\CommonwealthPlugins.txt') 'Game Files\ReclamationDay\CommonwealthPlugins.txt' 'gamefiles'
# The F2 overlay's WORLD SETTINGS page (WorldSettings.cpp reads it from Data\ReclamationDay, where the launcher copies it)
Add-File (Join-Path $gameFiles 'ReclamationDay\WorldSettings.json') 'Game Files\ReclamationDay\WorldSettings.json' 'gamefiles'

# Every file of ours in GameFiles has to be in a pack, as PackageFalloutBeta.ps1 copies them all: one left out above never
# reaches players updated by their launcher (1.6.7-245 went out without RD_Camps.esp, RD_Nemeses.esp and their files).
# README.txt is the zip's own, with its version filled in.
$gameFilesRoot = (Resolve-Path $gameFiles).Path
$packed = @{}
foreach ($file in $files) { $packed[$file.Path.ToLowerInvariant()] = $true }
$left = @(Get-ChildItem $gameFilesRoot -Recurse -File | Where-Object Name -ne 'README.txt' |
    ForEach-Object { Join-Path 'Game Files' $_.FullName.Substring($gameFilesRoot.Length + 1) } |
    Where-Object { -not $packed.ContainsKey($_.ToLowerInvariant()) })
if ($left) { throw "in GameFiles but in no pack (add them above): $($left -join ', ')" }

# The manifest and the packs (entry names are the paths in the install, with forward slashes)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$out = Join-Path $stage 'release'
New-Item -ItemType Directory -Force $out | Out-Null
$manifest = New-Object System.Text.StringBuilder
[void]$manifest.AppendLine("version $version")
foreach ($pack in ($files | Select-Object -ExpandProperty Pack -Unique)) {
    $zipPath = Join-Path $out "$pack.zip"
    $zip = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
    try {
        foreach ($file in ($files | Where-Object Pack -eq $pack)) {
            $entryName = $file.Path -replace '\\', '/'
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.Target, $entryName, 'Optimal')
            $hash = (Get-FileHash $file.Target -Algorithm SHA256).Hash.ToLowerInvariant()
            [void]$manifest.AppendLine("file $hash $((Get-Item $file.Target).Length) $pack $entryName")
        }
    } finally { $zip.Dispose() }
}
[IO.File]::WriteAllText((Join-Path $out 'manifest.txt'), $manifest.ToString(), (New-Object Text.UTF8Encoding($false)))

$assets = Get-ChildItem $out -File
"{0}: {1} files in {2} packs, {3:N0} MB to upload" -f $version, $files.Count, ($assets.Count - 1), (($assets | Measure-Object Length -Sum).Sum / 1MB)
if ($DryRun) { "dry run: made $out, nothing uploaded"; return }

if (-not $Yes) {
    $answer = Read-Host "Publish $version to everyone (their launchers will offer it)? [y/N]"
    if ($answer -ne 'y') { 'not published'; return }
}

# Your own GitHub sign-in, from Git Credential Manager. Not piped: PowerShell can put a byte order mark in front, which
# git refuses ("credential missing protocol field").
$request = Join-Path $env:TEMP 'rd-credential-request.txt'
[IO.File]::WriteAllText($request, "protocol=https`nhost=github.com`n`n", (New-Object Text.UTF8Encoding($false)))
$credential = cmd /c "git credential fill < `"$request`""
Remove-Item $request
$password = ($credential | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
if (-not $password) { throw "no GitHub sign-in in Git Credential Manager" }
$headers = @{ Authorization = "Bearer $password"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }

# A draft first: launchers only see the release once every file is up
$body = @{ tag_name = $version; name = $version; body = $Notes; draft = $true } | ConvertTo-Json
$release = Invoke-RestMethod -Method Post -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers -Body $body -ContentType 'application/json'
# Each pack with Windows' own curl, which streams the file: Invoke-RestMethod sent it as one long request, and a dropped
# connection failed the whole release (1.6.7-263's 830 MB nature pack). A failed pack is tried again, after removing
# what of it reached GitHub (a pack keeps its name). The sign-in goes to curl in a file, not on its command line.
$headerFile = Join-Path $env:TEMP 'rd-upload-headers.txt'
[IO.File]::WriteAllText($headerFile, "Authorization: Bearer $password`r`nAccept: application/vnd.github+json`r`nX-GitHub-Api-Version: 2022-11-28`r`nContent-Type: application/octet-stream`r`n",
    (New-Object Text.UTF8Encoding($false)))
try {
    foreach ($asset in $assets) {
        $uploadUri = "https://uploads.github.com/repos/$Repo/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($asset.Name))"
        for ($attempt = 1; ; $attempt++) {
            "uploading $($asset.Name) ({0:N1} MB){1}" -f ($asset.Length / 1MB), $(if ($attempt -gt 1) { ", try $attempt" } else { '' })
            # given up only when under 1 kB/s for 2 minutes, not on a slow but moving upload. curl's error text comes on
            # stderr, which 'Stop' would turn into a terminating error before a retry: 'Continue' around it.
            $ErrorActionPreference = 'Continue'
            $result = & curl.exe --silent --show-error --fail-with-body --connect-timeout 30 --speed-limit 1024 --speed-time 120 -X POST -H "@$headerFile" `
                -T $asset.FullName $uploadUri 2>&1
            $ErrorActionPreference = 'Stop'
            if ($LASTEXITCODE -eq 0) { break }
            "  failed (curl {0}): {1}" -f $LASTEXITCODE, (($result | Out-String).Trim() -replace '\s+', ' ')
            $existing = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/$($release.id)/assets?per_page=100" -Headers $headers
            foreach ($stale in @($existing | Where-Object name -eq $asset.Name)) {
                Invoke-RestMethod -Method Delete -Uri "https://api.github.com/repos/$Repo/releases/assets/$($stale.id)" -Headers $headers | Out-Null
            }
            if ($attempt -ge 4) { throw "$($asset.Name) failed $attempt times" }
            Start-Sleep -Seconds (15 * $attempt)
        }
    }
} catch {
    Invoke-RestMethod -Method Delete -Uri "https://api.github.com/repos/$Repo/releases/$($release.id)" -Headers $headers | Out-Null
    throw "upload failed, the draft was removed: $_"
} finally {
    Remove-Item $headerFile -ErrorAction SilentlyContinue
}

$body = @{ draft = $false; make_latest = 'true' } | ConvertTo-Json
Invoke-RestMethod -Method Patch -Uri "https://api.github.com/repos/$Repo/releases/$($release.id)" -Headers $headers -Body $body -ContentType 'application/json' | Out-Null
"published $version"
Remove-Item $stage -Recurse -Force
