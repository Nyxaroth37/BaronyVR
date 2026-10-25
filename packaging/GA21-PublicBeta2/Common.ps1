$ErrorActionPreference = 'Stop'
$script:Product = 'Barony VR GA21 Public Beta 1'
$script:RetailHash = '8566DA37BC39EA5A1ED08A8AD57608AF4F019FB415869258FB3C1D310B4419E4'
function Check-Payload([string]$Root) {
    $manifest = Get-Content -LiteralPath (Join-Path $Root 'payload.json') -Raw | ConvertFrom-Json
    foreach ($entry in $manifest.files) {
        $relative = [string]$entry.path
        if ($relative -match '(^[\\/]|:|(^|[\\/])\.\.([\\/]|$))') { throw 'Invalid package path.' }
        $path = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "Package file is missing or changed: $relative. Extract a fresh copy of the beta download."
        }
    }
    return $manifest
}
function Check-Retail([string]$Retail) {
    foreach ($name in @('barony.exe', 'steam_api64.dll', 'fmod.dll', 'models\models.txt')) {
        if (!(Test-Path -LiteralPath (Join-Path $Retail $name) -PathType Leaf)) {
            throw "Choose the Steam Barony folder containing $name."
        }
    }
    if ((Get-FileHash -LiteralPath (Join-Path $Retail 'barony.exe') -Algorithm SHA256).Hash -ne $script:RetailHash) {
        throw 'This retail Barony version has not been verified with this beta. This candidate supports Steam build 22630456. No game files were replaced.'
    }
}
function Check-Prerequisites {
    if (![Environment]::Is64BitProcess) { throw 'Run this beta using 64-bit Windows PowerShell on 64-bit Windows.' }
    foreach ($name in @('MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $name))) {
            throw "Microsoft Visual C++ x64 runtime is missing ($name). Install the Microsoft Visual C++ 2015-2022 or later x64 Redistributable, then retry."
        }
    }
}
function Check-XrRuntime {
    $active = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction SilentlyContinue).ActiveRuntime
    if (!$active -or !(Test-Path -LiteralPath $active -PathType Leaf)) {
        throw 'No active 64-bit OpenXR runtime was found. Set up your PC VR software and make it the active OpenXR runtime before playing.'
    }
}
function Find-Retail {
    $roots = @()
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        $value = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($value.SteamPath) { $roots += [string]$value.SteamPath }
        if ($value.InstallPath) { $roots += [string]$value.InstallPath }
    }
    if (${env:ProgramFiles(x86)}) { $roots += Join-Path ${env:ProgramFiles(x86)} 'Steam' }
    $libraries = @($roots)
    foreach ($root in @($roots | Select-Object -Unique)) {
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($line in [IO.File]::ReadAllLines($vdf)) {
                if ($line -match '^\s*"path"\s+"([^"]+)"') { $libraries += $Matches[1].Replace('\\', '\') }
            }
        }
    }
    foreach ($library in @($libraries | Select-Object -Unique)) {
        $manifest = Join-Path $library 'steamapps\appmanifest_371970.acf'
        if (Test-Path -LiteralPath $manifest) {
            foreach ($line in [IO.File]::ReadAllLines($manifest)) {
                if ($line -match '^\s*"installdir"\s+"([^"]+)"') {
                    if ($Matches[1] -match '[\\/:]|\.\.') { continue }
                    $candidate = Join-Path (Join-Path $library 'steamapps\common') $Matches[1]
                    if (Test-Path -LiteralPath (Join-Path $candidate 'barony.exe')) { return $candidate }
                }
            }
        }
    }
    return ''
}
function Seed-Defaults([string]$Root, [string]$Retail) {
    # Write a complete sibling file, then move without replacement. Existing
    # calibration is never overwritten, even if it appears during setup.
    $created = 0
    foreach ($name in @('vr_equipment_calibration.cfg', 'vr_body_calibration.cfg', 'vr_melee_calibration.cfg', 'vr_menu_pointer_alignment.cfg', 'vr_settings.cfg')) {
        $destination = Join-Path $Retail $name
        if (Test-Path -LiteralPath $destination) { continue }
        $bytes = [IO.File]::ReadAllBytes((Join-Path $Root "DefaultProfile\$name"))
        $temporary = Join-Path $Retail ('.baronyvr-default-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        $stream = $null
        try {
            $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            $stream.Dispose()
            $stream = $null
            try {
                [IO.File]::Move($temporary, $destination)
                $created++
            } catch [IO.IOException] {
                if (!(Test-Path -LiteralPath $destination -PathType Leaf)) { throw }
            }
        } finally {
            if ($stream) { $stream.Dispose() }
            if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) }
        }
    }
    return $created
}
