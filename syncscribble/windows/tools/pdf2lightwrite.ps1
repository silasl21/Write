param(
    [string]$InputPdf,
    [string]$OutputPath,
    [int]$Dpi = 130
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($InputPdf)) {
    [Console]::Error.WriteLine('Usage: pdf2lightwrite <input.pdf> [output.svgz] [dpi]')
    exit 1
}

function Find-PopplerExe([string]$ExeName) {
    $candidates = @(
        (Join-Path $PSScriptRoot $ExeName),
        (Join-Path $PSScriptRoot "poppler\Library\bin\$ExeName"),
        (Join-Path $PSScriptRoot "poppler\bin\$ExeName")
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    $cmd = Get-Command $ExeName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $wingetRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path -LiteralPath $wingetRoot) {
        $pkgs = Get-ChildItem -LiteralPath $wingetRoot -Filter 'oschwartz10612.Poppler*' -Directory -ErrorAction SilentlyContinue
        foreach ($pkg in $pkgs) {
            $hit = Get-ChildItem -LiteralPath $pkg.FullName -Filter $ExeName -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

$pdftoppm = Find-PopplerExe 'pdftoppm.exe'
if (-not $pdftoppm) {
    [Console]::Error.WriteLine('Poppler (pdftoppm.exe) not found. Install it with: winget install oschwartz10612.Poppler')
    exit 1
}

$cwd = (Get-Location).Path
$inFull = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($cwd, $InputPdf))
if (-not (Test-Path -LiteralPath $inFull)) {
    [Console]::Error.WriteLine("Input PDF not found: $inFull")
    exit 1
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($inFull)
    $dir = [System.IO.Path]::GetDirectoryName($inFull)
    $outFull = Join-Path $dir "${stem}_write.svgz"
} else {
    $outFull = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($cwd, $OutputPath))
}

$pageW = 1275
$pageH = 1650
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("pdf2lightwrite_" + [System.Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($tempDir) | Out-Null

try {
    Write-Output "Extracting pages from $inFull at $Dpi DPI..."
    & $pdftoppm -jpeg -r $Dpi $inFull (Join-Path $tempDir 'page')
    if ($LASTEXITCODE -ne 0) {
        [Console]::Error.WriteLine("pdftoppm failed with exit code $LASTEXITCODE")
        exit $LASTEXITCODE
    }

    $images = Get-ChildItem -LiteralPath $tempDir -Filter 'page-*.jpg' | Sort-Object {
        if ($_.BaseName -match '(\d+)$') { [int]$matches[1] } else { 0 }
    }
    if (-not $images -or $images.Count -eq 0) {
        [Console]::Error.WriteLine('No page images were produced by pdftoppm.')
        exit 2
    }

    $outDir = [System.IO.Path]::GetDirectoryName($outFull)
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
        [System.IO.Directory]::CreateDirectory($outDir) | Out-Null
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $fs = [System.IO.File]::Create($outFull)
    try {
        $gz = New-Object System.IO.Compression.GZipStream($fs, [System.IO.Compression.CompressionLevel]::Optimal)
        try {
            $sw = New-Object System.IO.StreamWriter($gz, $utf8NoBom)
            try {
                $sw.NewLine = "`n"
                $sw.WriteLine('<svg id="write-document" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">')
                $sw.WriteLine('<rect id="write-doc-background" width="100%" height="100%" fill="#808080"/>')

                foreach ($img in $images) {
                    $b64 = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($img.FullName))
                    $sw.WriteLine("<svg class=`"write-page`" color-interpolation=`"linearRGB`" x=`"10`" y=`"10`" width=`"${pageW}px`" height=`"${pageH}px`" xmlns=`"http://www.w3.org/2000/svg`" xmlns:xlink=`"http://www.w3.org/1999/xlink`">")
                    $sw.WriteLine('  <g class="write-content write-v3" xruling="0" yruling="40" marginLeft="100" papercolor="#FFFFFF" rulecolor="#9F0000FF">')
                    $sw.WriteLine('    <g class="ruleline write-no-dup" shape-rendering="crispEdges">')
                    $sw.WriteLine("      <rect class=`"pagerect`" fill=`"#FFFFFF`" stroke=`"none`" x=`"0`" y=`"0`" width=`"$pageW`" height=`"$pageH`" />")
                    $sw.WriteLine("      <image x=`"0`" y=`"0`" width=`"$pageW`" height=`"$pageH`" xlink:href=`"data:image/jpeg;base64,$b64`"/>")
                    $sw.WriteLine('    </g>')
                    $sw.WriteLine('  </g>')
                    $sw.WriteLine('</svg>')
                }

                $sw.WriteLine('</svg>')
            } finally {
                $sw.Dispose()
            }
        } finally {
            $gz.Dispose()
        }
    } finally {
        $fs.Dispose()
    }

    Write-Output "Done! Successfully created: $outFull"
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
