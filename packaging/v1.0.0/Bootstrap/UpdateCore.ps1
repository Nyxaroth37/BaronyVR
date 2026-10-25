$ErrorActionPreference = 'Stop'
$script:UpdateRepository = 'Nyxaroth37/BaronyVR'
$script:UpdateProtocol = 1
function Install-LauncherShortcut([string]$Directory,[string]$ManagedRoot,[string]$LauncherRoot) {
    if (!$Directory) { return }
    $shell=New-Object -ComObject WScript.Shell
    $target=Join-Path $LauncherRoot 'BaronyVR.exe'
    $oldPath=Join-Path $Directory 'Barony VR - GA21 Beta.lnk'
    $newPath=Join-Path $Directory 'BaronyVR.lnk'
    $known=@((Join-Path $env:LOCALAPPDATA 'BaronyVR\GA21-PublicBeta1\BaronyVR.exe'),(Join-Path $env:LOCALAPPDATA 'BaronyVR\GA21-Beta1R1\BaronyVR.exe'),$target)
    # Validate both shortcuts before changing either one.
    foreach ($path in @($oldPath,$newPath)) {
        if (Test-Path -LiteralPath $path) {
            $existing=$shell.CreateShortcut($path)
            if ($existing.TargetPath -notin $known -or $existing.Arguments) { throw 'An unrelated shortcut uses a BaronyVR shortcut name; it was preserved.' }
        }
    }
    $shortcut=$shell.CreateShortcut($newPath)
    $shortcut.TargetPath=$target;$shortcut.Arguments='';$shortcut.WorkingDirectory=$LauncherRoot
    $shortcut.Description='BaronyVR - native OpenXR for retail Barony'
    $shortcut.IconLocation=(Join-Path $LauncherRoot 'Barony.ico')+',0'
    $shortcut.Save()
    if (Test-Path -LiteralPath $oldPath) { [IO.File]::Delete($oldPath) }
}
function Safe-Leaf([string]$Name) {
    return $Name -match '^[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$' -and $Name -notmatch '\.\.' -and
        $Name -notmatch '(?i)^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)' -and !$Name.EndsWith('.')
}
function Safe-Relative([string]$Relative) {
    if (!$Relative -or $Relative.Length -gt 220 -or $Relative -match '[\x00-\x1f:*?"<>|]' -or $Relative -match '^[\\/]') { return $false }
    foreach ($segment in ($Relative.Replace('\','/') -split '/')) {
        if (!$segment -or $segment -in @('.', '..') -or $segment.EndsWith('.') -or $segment.EndsWith(' ') -or
            $segment -match '(?i)^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') { return $false }
    }
    return $true
}
function Read-Json([string]$Path) { return ([IO.File]::ReadAllText($Path) | ConvertFrom-Json) }
function Hash-File([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Mark-Managed([string]$ManagedRoot, [string]$Directory, [string]$Kind) {
    $marker=Join-Path $Directory '.baronyvr-owned.json'
    if (Test-Path -LiteralPath $marker) { throw 'Managed directory marker already exists.' }
    Atomic-Json $marker @{ product='BaronyVR.UpdateProtocol1'; home=[IO.Path]::GetFullPath($ManagedRoot); kind=$Kind }
}
function New-ManagedDirectory([string]$ManagedRoot,[string]$Bucket) {
    if ($Bucket -notin @('Staging','Checks','Work')) { throw 'Invalid updater workspace.' }
    $directory=Join-Path (Join-Path $ManagedRoot $Bucket) ([Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    Mark-Managed $ManagedRoot $directory $Bucket
    return $directory
}
function Remove-ManagedDirectory([string]$ManagedRoot,[string]$Bucket,[string]$Directory) {
    if ($Bucket -notin @('Versions','Staging','Checks','Work','Legacy')) { throw 'Invalid cleanup bucket.' }
    $expectedParent=[IO.Path]::GetFullPath((Join-Path $ManagedRoot $Bucket)).TrimEnd('\')
    if ($Bucket -eq 'Legacy') {
        $expectedParent=Split-Path -Parent ([IO.Path]::GetFullPath($ManagedRoot).TrimEnd('\'))
        if ((Split-Path -Leaf $Directory) -cne 'GA21-PublicBeta1') { throw 'Only the verified legacy Beta 1 folder can be cleaned.' }
    }
    $full=[IO.Path]::GetFullPath($Directory).TrimEnd('\')
    if ((Split-Path -Parent $full) -ne $expectedParent -or !(Test-Path -LiteralPath $full -PathType Container)) { throw 'Cleanup path is outside its managed bucket.' }
    $marker=Read-Json (Join-Path $full '.baronyvr-owned.json')
    if ($marker.product -ne 'BaronyVR.UpdateProtocol1' -or $marker.kind -ne $Bucket -or $marker.home -ne [IO.Path]::GetFullPath($ManagedRoot)) { throw 'Cleanup ownership marker differs.' }
    # Inspect directories one level at a time: never recurse through a junction.
    $pending=New-Object 'System.Collections.Generic.Stack[string]';$pending.Push($full)
    while ($pending.Count) {
        $current=$pending.Pop()
        $directoryInfo=Get-Item -LiteralPath $current -Force
        if (($directoryInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Cleanup refuses linked folders.' }
        foreach ($item in Get-ChildItem -LiteralPath $current -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Cleanup refuses linked files or folders.' }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
    [IO.Directory]::Delete($full,$true)
}
function Test-LegacyContents([string]$Root,$Manifest) {
    $allowed=@{'payload.json'=$true;'installation.json'=$true;'.baronyvr-owned.json'=$true}
    foreach ($entry in $Manifest.files) { $allowed[([string]$entry.path).Replace('\','/')]=$true }
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File -Force) {
        $relative=$file.FullName.Substring($Root.Length+1).Replace('\','/')
        if ($allowed.ContainsKey($relative)) { continue }
        if ($relative -notmatch '^ShortcutBackups/Barony VR - GA21 Beta\.lnk\.[0-9a-f]{32}\.bak$' -or $file.Length -lt 76 -or $file.Length -gt 256KB) { return $false }
        $expected=[byte[]]@(76,0,0,0,1,20,2,0,0,0,0,0,192,0,0,0,0,0,0,70)
        $header=New-Object byte[] 20
        $stream=[IO.File]::OpenRead($file.FullName)
        try { $read=$stream.Read($header,0,20) } finally { $stream.Dispose() }
        if ($read -ne 20) { return $false }
        for ($index=0;$index -lt 20;$index++) { if ($header[$index] -ne $expected[$index]) { return $false } }
    }
    return $true
}
function Verify-Bootstrap([string]$Root) {
    $manifest=Read-Json (Join-Path $Root 'bootstrap-manifest.json')
    $allowed=@{'bootstrap-manifest.json'=$true}
    foreach ($entry in $manifest.files) {
        if (!(Safe-Leaf ([string]$entry.name)) -or [string]$entry.sha256 -notmatch '^[A-Fa-f0-9]{64}$') { throw 'Invalid launcher manifest.' }
        $allowed[[string]$entry.name]=$true
        if ((Hash-File (Join-Path $Root $entry.name)) -ne $entry.sha256) { throw 'Launcher file differs from its manifest.' }
    }
    foreach ($file in Get-ChildItem -LiteralPath $Root -Force) {
        if ($file.PSIsContainer -or !$allowed.ContainsKey($file.Name) -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected files or links in launcher folder; it was retained.' }
    }
}
function Clean-PreviousLauncher([string]$ManagedRoot) {
    $previous=Join-Path $ManagedRoot 'Launcher.previous'
    if (!(Test-Path -LiteralPath $previous)) { return }
    if ((Get-Item -LiteralPath $previous -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Previous launcher folder is linked; it was retained.' }
    $known=@('20768979A90B76F0369BA02E7DED266B723F02444F2204709928A0922B02661F','B6E778497429662C266BE4055CB2B54A8FBDB615920A8E9EF18677A7E6A69935','B4E41C7623CA3C386952F58C0A7944E05A8921027B3BDC54103536A7DD195529')
    if ((Hash-File (Join-Path $previous 'bootstrap-manifest.json')) -notin $known) { throw 'Previous launcher identity differs; it was retained.' }
    Verify-Bootstrap $previous
    [IO.Directory]::Delete($previous,$true)
    return 'Removed the verified previous launcher backup.'
}
function Clean-LegacyInstallation([string]$ManagedRoot) {
    $base=Split-Path -Parent ([IO.Path]::GetFullPath($ManagedRoot).TrimEnd('\'))
    $legacy=Join-Path $base 'GA21-PublicBeta1'
    if (!(Test-Path -LiteralPath (Join-Path $legacy '.baronyvr-owned.json'))) { return }
    # A verified rollback runtime must remain available after the old flat
    # install is removed. It can be Beta 1 or a newer retained working build.
    $state=Read-Json (Join-Path $ManagedRoot 'active.json')
    if (!$state.previous) { return 'Retained original Beta 1: no rollback version is available.' }
    $null=Verify-Record $ManagedRoot $state.previous
    $expected='11B7506E973134EDA161E8F5658469050806EAFFD61D21C0B430617031357E40'
    if ((Hash-File (Join-Path $legacy 'payload.json')) -ne $expected) { return 'Retained original Beta 1: manifest differs.' }
    $manifest=Verify-Payload $legacy -Legacy
    if (!(Test-LegacyContents $legacy $manifest)) { return 'Retained original Beta 1: unrecognized personal files or invalid shortcut backups.' }
    Remove-ManagedDirectory $ManagedRoot 'Legacy' $legacy
    return 'Removed the verified original Beta 1 installation and installer-created shortcut backups.'
}
function Clean-ManagedFiles([string]$ManagedRoot) {
    $messages=@()
    $state=Read-Json (Join-Path $ManagedRoot 'active.json')
    $keep=@([string]$state.active.version)
    if ($state.previous) { $keep += [string]$state.previous.version }
    $versions=Join-Path $ManagedRoot 'Versions'
    if (Test-Path -LiteralPath $versions) {
        foreach ($directory in Get-ChildItem -LiteralPath $versions -Directory -Force) {
            if ($directory.Name -in $keep) { continue }
            try {
                $manifest=Read-Json (Join-Path $directory.FullName 'payload.json')
                $null=Verify-Payload $directory.FullName -Legacy:($manifest.version -eq 'GA21-PublicBeta1')
                $allowed=@{'payload.json'=$true;'installation.json'=$true;'.baronyvr-owned.json'=$true}
                foreach ($entry in $manifest.files) { $allowed[([string]$entry.path).Replace('\','/')]=$true }
                foreach ($file in Get-ChildItem -LiteralPath $directory.FullName -File -Recurse -Force) {
                    if (!$allowed.ContainsKey($file.FullName.Substring($directory.FullName.Length+1).Replace('\','/'))) { throw 'Unrecognized personal files were found; version folder was retained.' }
                }
                Remove-ManagedDirectory $ManagedRoot 'Versions' $directory.FullName
            } catch { $messages += 'Retained '+$directory.Name+': '+$_.Exception.Message }
        }
    }
    foreach ($bucket in @('Staging','Checks','Work')) {
        $path=Join-Path $ManagedRoot $bucket
        if (!(Test-Path -LiteralPath $path)) { continue }
        $position=0
        foreach ($directory in @(Get-ChildItem -LiteralPath $path -Directory -Force | Sort-Object LastWriteTimeUtc -Descending)) {
            # Recent worker directories may still be in use. The next launch
            # sweeps abandoned work after 24 hours; successful jobs clean now.
            $position++
            if ($position -le 3 -and $directory.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddHours(-24)) { continue }
            try { Remove-ManagedDirectory $ManagedRoot $bucket $directory.FullName } catch { $messages += 'Retained updater work: '+$_.Exception.Message }
        }
    }
    return $messages
}
function Atomic-Json([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $temporary = Join-Path $parent ('.state-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $Path) {
            $backup = $Path + '.previous'
            if (Test-Path -LiteralPath $backup -PathType Leaf) { [IO.File]::Delete($backup) }
            [IO.File]::Replace($temporary, $Path, $backup)
        } else { [IO.File]::Move($temporary, $Path) }
    } finally { if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) } }
}
function Verify-Payload([string]$Root, [switch]$Legacy) {
    $manifestPath = Join-Path $Root 'payload.json'
    if (!(Test-Path -LiteralPath $manifestPath) -or (Get-Item -LiteralPath $manifestPath).Length -gt 1MB) { throw 'Missing or oversized payload manifest.' }
    $manifest = Read-Json $manifestPath
    if (!(Safe-Leaf ([string]$manifest.version))) { throw 'Invalid package version.' }
    if (!$Legacy -and ($manifest.update_protocol -ne 1 -or $manifest.release_sequence -lt 2 -or $manifest.release_sequence -gt 1000000)) { throw 'Unsupported update package format.' }
    $seen = @{}
    $entries = @($manifest.files)
    if ($entries.Count -lt 10 -or $entries.Count -gt 1000) { throw 'Unexpected payload file count.' }
    foreach ($entry in $entries) {
        $relative = ([string]$entry.path).Replace('\','/')
        if (!(Safe-Relative $relative) -or $relative -in @('payload.json','installation.json') -or $seen.ContainsKey($relative)) { throw 'Invalid or duplicate payload path.' }
        if ([string]$entry.sha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Invalid payload hash.' }
        $seen[$relative] = $true
        $path = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Hash-File $path) -ne $entry.sha256) { throw "Missing or changed package file: $relative" }
    }
    foreach ($required in @('Runtime/barony.exe','Common.ps1','DefaultProfile/vr_equipment_calibration.cfg')) {
        if (!$seen.ContainsKey($required)) { throw "Required update file is missing: $required" }
    }
    $gameScript = 'LaunchGame.ps1'
    if ($Legacy) { $gameScript = 'Launch.ps1' }
    if (!$seen.ContainsKey($gameScript)) { throw 'Game launch script is missing.' }
    return $manifest
}
function Version-Root([string]$ManagedRoot, [string]$Version) {
    if (!(Safe-Leaf $Version)) { throw 'Invalid installed version reference.' }
    return Join-Path (Join-Path $ManagedRoot 'Versions') $Version
}
function Verify-Record([string]$ManagedRoot, $Record) {
    if (!$Record -or !(Safe-Leaf ([string]$Record.version)) -or [string]$Record.manifest_sha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Invalid launcher version record.' }
    $root = Version-Root $ManagedRoot ([string]$Record.version)
    if ((Hash-File (Join-Path $root 'payload.json')) -ne $Record.manifest_sha256) { throw 'Installed version manifest changed.' }
    $manifest = Verify-Payload $root -Legacy:($Record.sequence -eq 1)
    if ($manifest.version -ne $Record.version) { throw 'Installed version identity differs.' }
    return $root
}
function Make-Record([string]$Root, [int]$Sequence, [string]$Tag) {
    $manifest = Read-Json (Join-Path $Root 'payload.json')
    return [pscustomobject]@{ version=[string]$manifest.version; sequence=$Sequence; tag=$Tag; manifest_sha256=(Hash-File (Join-Path $Root 'payload.json')) }
}
function Activate-Version([string]$ManagedRoot, $Record) {
    $null = Verify-Record $ManagedRoot $Record
    $statePath = Join-Path $ManagedRoot 'active.json'
    $previous = $null
    if (Test-Path -LiteralPath $statePath) {
        $old = Read-Json $statePath
        if ($old.active.version -eq $Record.version) { return }
        $null = Verify-Record $ManagedRoot $old.active
        $previous = $old.active
    }
    Atomic-Json $statePath ([ordered]@{ active=$Record; previous=$previous })
}
function Rollback-Version([string]$ManagedRoot) {
    $path = Join-Path $ManagedRoot 'active.json'
    $state = Read-Json $path
    if (!$state.previous) { throw 'No previous version is available.' }
    $null = Verify-Record $ManagedRoot $state.previous
    Atomic-Json $path ([ordered]@{ active=$state.previous; previous=$state.active })
}
function Extract-CheckedZip([string]$Zip, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $Destination) { throw 'Extraction destination already exists.' }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        if ($archive.Entries.Count -gt 1200) { throw 'Too many files in update archive.' }
        $total = 0L; $seen = @{}; $rootName = $null
        # Validate the entire archive before writing any of its files.
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\','/')
            $trimmed = $name.TrimEnd('/')
            if (!(Safe-Relative $trimmed)) { throw 'Unsafe update archive path.' }
            $parts = $trimmed -split '/'
            if (!(Safe-Leaf $parts[0])) { throw 'Invalid update root folder.' }
            if (!$rootName) { $rootName=$parts[0] }
            if ($parts[0] -cne $rootName -or $seen.ContainsKey($trimmed)) { throw 'Duplicate or multiple update archive roots.' }
            $seen[$trimmed]=$true
            if ((($entry.ExternalAttributes -shr 16) -band 61440) -eq 40960) { throw 'Update archive links are not supported.' }
            $total += $entry.Length
            if ($entry.Length -gt 128MB -or $total -gt 256MB) { throw 'Update archive is too large.' }
        }
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\','/')
            $path = Join-Path $Destination $name
            if ($name.EndsWith('/')) {
                if (!(Test-Path -LiteralPath $path)) { New-Item -ItemType Directory -Path $path -Force | Out-Null }
            } else {
                $parent=Split-Path -Parent $path
                if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
                $archiveStream=$entry.Open(); $output=$null
                try {
                    $output=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
                    $archiveStream.CopyTo($output)
                } finally { if ($output) { $output.Dispose() }; $archiveStream.Dispose() }
            }
        }
        if (!$rootName) { throw 'Empty update archive.' }
        return Join-Path $Destination $rootName
    } finally { $archive.Dispose() }
}
function Http-Json([string]$Uri) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    return Invoke-RestMethod -Uri $Uri -Headers @{'User-Agent'='BaronyVR-Launcher/1';'Accept'='application/vnd.github+json'} -TimeoutSec 10
}
function Download-ReleaseAsset($Asset, [string]$Destination, [long]$Limit=128MB) {
    $prefix='https://github.com/' + $script:UpdateRepository + '/releases/download/'
    if (![string]$Asset.browser_download_url -or !([string]$Asset.browser_download_url).StartsWith($prefix,[StringComparison]::Ordinal) -or
        $Asset.state -ne 'uploaded' -or $Asset.size -le 0 -or $Asset.size -gt $Limit -or [string]$Asset.digest -notmatch '^sha256:[0-9a-fA-F]{64}$') { throw 'Release asset metadata is invalid or unverified.' }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $Asset.browser_download_url -Headers @{'User-Agent'='BaronyVR-Launcher/1'} -OutFile $Destination -TimeoutSec 45 | Out-Null
    if ((Get-Item -LiteralPath $Destination).Length -ne $Asset.size -or (Hash-File $Destination) -ne ([string]$Asset.digest).Substring(7)) { throw 'Release download failed SHA256 or size verification.' }
}
function Find-Update([int]$CurrentSequence, [string]$Scratch) {
    # Invoke-RestMethod emits its JSON array as one pipeline item in Windows
    # PowerShell. Assign it directly so foreach enumerates releases, not an
    # extra array wrapper whose draft property becomes an array of booleans.
    $releases = Http-Json ('https://api.github.com/repos/' + $script:UpdateRepository + '/releases?per_page=10')
    $best = $null
    foreach ($release in $releases) {
        if ($release.draft -or $release.prerelease) { continue }
        $descriptor = @($release.assets | Where-Object { $_.name -eq 'baronyvr-update.json' })
        if ($descriptor.Count -ne 1) { continue }
        $path=Join-Path $Scratch ([Guid]::NewGuid().ToString('N')+'.json')
        try {
            Download-ReleaseAsset $descriptor[0] $path 64KB
            $update=Read-Json $path
            if ($update.protocol -ne 1 -or !(Safe-Leaf ([string]$update.version)) -or $update.sequence -le $CurrentSequence -or $update.sequence -gt 1000000 -or
                $update.tag -cne $release.tag_name -or [string]$update.package_sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
                [string]$update.package_name -notmatch '^BaronyVR_[A-Za-z0-9_.-]+_Windows\.zip$') { continue }
            $package=@($release.assets | Where-Object { $_.name -ceq $update.package_name })
            if ($package.Count -ne 1 -or [string]$package[0].digest -notmatch '^sha256:[0-9a-fA-F]{64}$' -or
                ([string]$package[0].digest).Substring(7) -ne $update.package_sha256) { continue }
            if (!$best -or $update.sequence -gt $best.update.sequence) {
                $best=[pscustomobject]@{ update=$update; asset=$package[0]; release_url=[string]$release.html_url; name=[string]$release.name }
            }
        } finally { if (Test-Path -LiteralPath $path) { [IO.File]::Delete($path) } }
    }
    return $best
}
function Install-VerifiedPackage([string]$ManagedRoot, [string]$Zip, $Update) {
    if (Get-Process -Name barony -ErrorAction SilentlyContinue) { throw 'Close Barony before applying an update.' }
    if ($Update.min_launcher -gt $script:UpdateProtocol) { throw 'This release requires a newer launcher. Download its installer from GitHub Releases.' }
    if ((Hash-File $Zip) -ne $Update.package_sha256) { throw 'Update package SHA256 mismatch.' }
    $scratch=New-ManagedDirectory $ManagedRoot 'Staging'
    $package=Extract-CheckedZip $Zip (Join-Path $scratch 'Extracted')
    $manifest=Verify-Payload $package
    if ($manifest.version -cne $Update.version -or $manifest.release_sequence -ne $Update.sequence) { throw 'Downloaded package version differs from release metadata.' }
    $listed=@{}
    foreach ($entry in $manifest.files) { $listed[([string]$entry.path).Replace('\','/')]=$true }
    foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
        $relative=$file.FullName.Substring($package.Length+1).Replace('\','/')
        if ($relative -ne 'payload.json' -and !$listed.ContainsKey($relative)) { throw 'Unlisted file in update archive.' }
    }
    $destination=Version-Root $ManagedRoot ([string]$Update.version)
    if (Test-Path -LiteralPath $destination) {
        $null=Verify-Payload $destination
        if ((Hash-File (Join-Path $destination 'payload.json')) -ne (Hash-File (Join-Path $package 'payload.json'))) { throw 'An installed version already uses this name with different contents.' }
    } else {
        $parent=Split-Path -Parent $destination
        if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        $configuration=Read-Json (Join-Path $ManagedRoot 'configuration.json')
        Atomic-Json (Join-Path $package 'installation.json') @{retail=[string]$configuration.retail}
        [IO.Directory]::Move($package,$destination)
        Mark-Managed $ManagedRoot $destination 'Versions'
    }
    $record=Make-Record $destination ([int]$Update.sequence) ([string]$Update.tag)
    Activate-Version $ManagedRoot $record
    Remove-ManagedDirectory $ManagedRoot 'Staging' $scratch
    return $record
}
