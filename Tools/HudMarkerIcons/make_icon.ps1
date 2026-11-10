param(
    # The icon: a PNG, the shape in any colour on a transparent background
    [Parameter(Mandatory)][string]$Png,
    # The C++ name of the table
    [Parameter(Mandatory)][string]$Name,
    # How many scan lines the icon is drawn in (the HUD's look; too many and they blur together at the marker's size)
    [int]$Lines = 16,
    # The share of each line that's drawn (the rest is the gap between lines)
    [double]$Fill = 0.7,
    # Narrower spans than this (grid units) are left out: a line's sliver at a point reads as a speck
    [double]$MinSpan = 0.8
)

# Turns a HUD marker icon (Code/client/Games/Fallout4/Interface/HudMarker.cpp) from a PNG into the scan lines it's
# drawn with: for each line, the spans of the shape at its top and bottom edges, as quads on the marker's 32 unit grid
# (centred, y down). Prints the C++ table to paste into HudMarker.cpp.
#   powershell -ExecutionPolicy Bypass -File Tools\HudMarkerIcons\make_icon.ps1 -Png Tools\HudMarkerIcons\climb.png -Name kClimbLines

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap (Resolve-Path $Png).Path
$w = $bmp.Width; $h = $bmp.Height
$data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($data.Stride * $h)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$bmp.UnlockBits($data); $bmp.Dispose()

# the shape: opaque pixels; the source's own scan line gaps closed (a pixel is in it with an opaque one this close above
# or below)
$opaque = New-Object bool[] ($w * $h)
for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) { $opaque[$y * $w + $x] = $bytes[$y * $data.Stride + $x * 4 + 3] -gt 128 } }
$close = 3
$solid = New-Object bool[] ($w * $h)
$minX = $w; $maxX = -1; $minY = $h; $maxY = -1
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $above = $false; $below = $false
        for ($d = 0; $d -le $close; $d++) {
            if ($y - $d -ge 0 -and $opaque[($y - $d) * $w + $x]) { $above = $true }
            if ($y + $d -lt $h -and $opaque[($y + $d) * $w + $x]) { $below = $true }
        }
        if ($above -and $below) {
            $solid[$y * $w + $x] = $true
            if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
if ($maxX -lt 0) { throw "no shape in $Png" }

# onto the grid: the larger side 32 units, centred
$size = [Math]::Max($maxX - $minX + 1, $maxY - $minY + 1)
$scale = 32.0 / $size
$minPixels = [Math]::Max(2, [int][Math]::Ceiling($MinSpan / $scale))

# the spans of the shape along a row: (start, end) pairs, ends exclusive
function Get-Spans([int]$Row) {
    $spans = @(); $start = -1
    for ($x = $minX; $x -le $maxX + 1; $x++) {
        $in = $x -le $maxX -and $solid[$Row * $w + $x]
        if ($in -and $start -lt 0) { $start = $x }
        elseif (-not $in -and $start -ge 0) { if ($x - $start -ge $minPixels) { $spans += , @($start, $x) }; $start = -1 }
    }
    , $spans
}
$cx = ($minX + $maxX + 1) / 2.0; $cy = ($minY + $maxY + 1) / 2.0
# (always with a decimal point: "6f" isn't a C++ literal)
$gx = { param($x) ([Math]::Round(($x - $cx) * $scale, 2)).ToString('0.0#', [Globalization.CultureInfo]::InvariantCulture) }
$gy = { param($y) ([Math]::Round(($y - $cy) * $scale, 2)).ToString('0.0#', [Globalization.CultureInfo]::InvariantCulture) }

$pitch = ($maxY - $minY + 1) / $Lines
$out = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $Lines; $i++) {
    $top = [int][Math]::Floor($minY + $i * $pitch)
    $bottom = [int][Math]::Min($maxY, [Math]::Floor($minY + $i * $pitch + $pitch * $Fill))
    $spansTop = Get-Spans $top
    $spansBottom = Get-Spans $bottom
    if ($spansTop.Count -eq 0 -and $spansBottom.Count -eq 0) { continue }
    # the same spans at both edges: quads that follow the shape's slopes; otherwise the middle's, straight
    if ($spansTop.Count -ne $spansBottom.Count -or $spansTop.Count -eq 0) {
        $middle = Get-Spans ([int](($top + $bottom) / 2))
        $spansTop = $middle; $spansBottom = $middle
    }
    for ($s = 0; $s -lt $spansTop.Count; $s++) {
        $out.Add(('    {{{0}f, {1}f, {2}f, {3}f, {4}f, {5}f}},' -f (& $gy $top), (& $gy ($bottom + 1)), (& $gx $spansTop[$s][0]), (& $gx $spansTop[$s][1]), (& $gx $spansBottom[$s][0]), (& $gx $spansBottom[$s][1])))
    }
}
"// From $([IO.Path]::GetFileName($Png)) by Tools/HudMarkerIcons/make_icon.ps1 -Lines $Lines -Fill $Fill -MinSpan $MinSpan"
"constexpr ScanLine $Name[] = {"
$out
"};"
