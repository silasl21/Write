$ErrorActionPreference = 'Stop'

$foreground = $false
$compress = $true
$vector = $false
$pdfIn = ''
$outFile = ''

for ($i = 0; $i -lt $args.Count; $i++) {
    $key = [string]$args[$i]
    switch -Exact ($key) {
        '-f'           { $foreground = $true }
        '--fg'         { $foreground = $true }
        '--foreground' { $foreground = $true }
        '--nozip'      { $compress = $false }
        '--vector'     { $vector = $true }
        '-o' {
            if ($i + 1 -ge $args.Count) { [Console]::Error.WriteLine('Missing argument after -o'); exit 1 }
            $i++; $outFile = [string]$args[$i]
        }
        '--output' {
            if ($i + 1 -ge $args.Count) { [Console]::Error.WriteLine('Missing argument after --output'); exit 1 }
            $i++; $outFile = [string]$args[$i]
        }
        default { $pdfIn = $key }
    }
}

if ([string]::IsNullOrWhiteSpace($pdfIn)) {
    Write-Output 'pdf2write: Convert PDF to svg document for Stylus Labs Write'
    Write-Output 'Usage: pdf2write [options] <PDF-file>'
    Write-Output '  -f,--fg,--foreground: place page images in editable layer instead of ruling layer'
    Write-Output '  -o,--output FILE:     specify output filename/path (default: <input>.svgz)'
    Write-Output '  --nozip:              generate uncompressed svg instead of svgz'
    Write-Output '  --vector:             convert to vectors instead of images (-f does not apply)'
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

$cwd = (Get-Location).Path
$inFull = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($cwd, $pdfIn))
if (-not (Test-Path -LiteralPath $inFull)) {
    [Console]::Error.WriteLine("Input PDF not found: $inFull")
    exit 1
}

if ([string]::IsNullOrWhiteSpace($outFile)) {
    $baseOut = Join-Path (Get-Location).Path ([System.IO.Path]::GetFileNameWithoutExtension($inFull) + '.svg')
} else {
    $trimmed = $outFile -replace '\.svgz$', '' -replace '\.svg$', ''
    $baseOut = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($cwd, $trimmed + '.svg'))
}
$finalOut = if ($compress) { $baseOut + 'z' } else { $baseOut }

$outDir = [System.IO.Path]::GetDirectoryName($finalOut)
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    [System.IO.Directory]::CreateDirectory($outDir) | Out-Null
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("pdf2write_" + [System.Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($tempDir) | Out-Null

try {
    if ($vector) {
        $pdftocairo = Find-PopplerExe 'pdftocairo.exe'
        if (-not $pdftocairo) {
            [Console]::Error.WriteLine('Poppler (pdftocairo.exe) not found. Install it with: winget install oschwartz10612.Poppler')
            exit 1
        }

        $pageFiles = New-Object System.Collections.Generic.List[string]
        for ($p = 1; $p -le 1000; $p++) {
            $pFile = Join-Path $tempDir "page_$p.svg"
            $errFile = Join-Path $tempDir "page_${p}_err.txt"
            $proc = Start-Process -FilePath $pdftocairo -ArgumentList @('-svg', '-f', "$p", '-l', "$p", $inFull, $pFile) -NoNewWindow -Wait -PassThru -RedirectStandardError $errFile
            if ($proc.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $pFile) -or (Get-Item -LiteralPath $pFile).Length -eq 0) {
                if ($p -eq 1) {
                    $errMsg = if (Test-Path -LiteralPath $errFile) { Get-Content -LiteralPath $errFile -Raw } else { '' }
                    [Console]::Error.WriteLine("pdftocairo failed on page 1 (exit $($proc.ExitCode)): $errMsg")
                    exit 1
                }
                break
            }
            $pageFiles.Add($pFile)
        }

        $fs = [System.IO.File]::Create($finalOut)
        try {
            $targetStream = if ($compress) { New-Object System.IO.Compression.GZipStream($fs, [System.IO.Compression.CompressionLevel]::Optimal) } else { $fs }
            try {
                foreach ($pf in $pageFiles) {
                    $bytes = [System.IO.File]::ReadAllBytes($pf)
                    $targetStream.Write($bytes, 0, $bytes.Length)
                }
            } finally {
                if ($compress) { $targetStream.Dispose() }
            }
        } finally {
            $fs.Dispose()
        }

        Write-Output "Finished creating $finalOut"
        exit 0
    }

    $pdftoppm = Find-PopplerExe 'pdftoppm.exe'
    if (-not $pdftoppm) {
        [Console]::Error.WriteLine('Poppler (pdftoppm.exe) not found. Install it with: winget install oschwartz10612.Poppler')
        exit 1
    }

    Write-Output "Converting $inFull to images..."
    & $pdftoppm -png -r 300 $inFull (Join-Path $tempDir 'out')
    if ($LASTEXITCODE -ne 0) {
        [Console]::Error.WriteLine("pdftoppm failed with exit code $LASTEXITCODE")
        exit $LASTEXITCODE
    }

    $pngPages = Get-ChildItem -LiteralPath $tempDir -Filter 'out-*.png' | Sort-Object {
        if ($_.BaseName -match '(\d+)$') { [int]$matches[1] } else { 0 }
    }
    if (-not $pngPages -or $pngPages.Count -eq 0) {
        [Console]::Error.WriteLine('No page images found from pdftoppm.')
        exit 2
    }

    Write-Output 'Generating Write document...'
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $fs = [System.IO.File]::Create($finalOut)
    try {
        $targetStream = if ($compress) { New-Object System.IO.Compression.GZipStream($fs, [System.IO.Compression.CompressionLevel]::Optimal) } else { $fs }
        try {
            $sw = New-Object System.IO.StreamWriter($targetStream, $utf8NoBom)
            try {
                $sw.NewLine = "`n"
                $sw.WriteLine('<svg id="write-document" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">')
                $sw.WriteLine('<rect id="write-doc-background" width="100%" height="100%" fill="#808080"/>')

                foreach ($png in $pngPages) {
                    $pngBytes = [System.IO.File]::ReadAllBytes($png.FullName)
                    if ($pngBytes.Length -lt 24) { throw "Invalid PNG file: $($png.FullName)" }
                    $w2 = ([uint32]$pngBytes[16] -shl 24) -bor ([uint32]$pngBytes[17] -shl 16) -bor ([uint32]$pngBytes[18] -shl 8) -bor [uint32]$pngBytes[19]
                    $h2 = ([uint32]$pngBytes[20] -shl 24) -bor ([uint32]$pngBytes[21] -shl 16) -bor ([uint32]$pngBytes[22] -shl 8) -bor [uint32]$pngBytes[23]
                    $w = [int]($w2 / 2)
                    $h = [int]($h2 / 2)
                    $b64 = [Convert]::ToBase64String($pngBytes)

                    $sw.WriteLine("<svg class=`"write-page`" color-interpolation=`"linearRGB`" x=`"10`" y=`"10`" width=`"${w}px`" height=`"${h}px`" xmlns=`"http://www.w3.org/2000/svg`" xmlns:xlink=`"http://www.w3.org/1999/xlink`">")
                    $sw.WriteLine('  <g class="write-content write-v3" xruling="0" yruling="40" marginLeft="100" papercolor="#FFFFFF" rulecolor="#9F0000FF">')
                    $sw.WriteLine('    <g class="ruleline write-no-dup" shape-rendering="crispEdges">')
                    $sw.WriteLine("      <rect class=`"pagerect`" fill=`"#FFFFFF`" stroke=`"none`" x=`"0`" y=`"0`" width=`"$w`" height=`"$h`" />")
                    if ($foreground) { $sw.WriteLine('    </g>') }
                    $sw.WriteLine("      <image x=`"0`" y=`"0`" width=`"$w`" height=`"$h`" xlink:href=`"data:image/png;base64,$b64`"/>")
                    if (-not $foreground) { $sw.WriteLine('    </g>') }
                    $sw.WriteLine('  </g>')
                    $sw.WriteLine('</svg>')
                }

                $sw.WriteLine('</svg>')
            } finally {
                $sw.Dispose()
            }
        } finally {
            if ($compress) { $targetStream.Dispose() }
        }
    } finally {
        $fs.Dispose()
    }

    Write-Output "Finished creating $finalOut"
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
