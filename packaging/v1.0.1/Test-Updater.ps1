$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Bootstrap\UpdateCore.ps1')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('BaronyVR-UpdaterTest-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$managed=Join-Path $testRoot 'Managed'
[IO.Directory]::CreateDirectory($managed) | Out-Null
Atomic-Json (Join-Path $managed 'configuration.json') @{retail=(Join-Path $testRoot 'RetailSentinel')}
$sentinel=Join-Path $testRoot 'RetailSentinel'
[IO.File]::WriteAllText($sentinel,'personal calibration and saves must survive')
function Assert([bool]$Condition,[string]$Message) { if (!$Condition) { throw "TEST FAILED: $Message" } }
function Reject([scriptblock]$Action,[string]$Message) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    Assert $rejected $Message
}
function Fixture([string]$Version,[int]$Sequence) {
    $container=Join-Path $testRoot ('fixture-'+[Guid]::NewGuid().ToString('N'))
    $root=Join-Path $container 'BaronyVR_Fixture'
    [IO.Directory]::CreateDirectory((Join-Path $root 'Runtime')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $root 'DefaultProfile')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'Runtime\barony.exe'),'non-executable updater test data')
    [IO.File]::WriteAllText((Join-Path $root 'DefaultProfile\vr_equipment_calibration.cfg'),'fixture defaults')
    foreach ($name in @('Common.ps1','LaunchGame.ps1','a.txt','b.txt','c.txt','d.txt','e.txt','f.txt','g.txt')) {
        [IO.File]::WriteAllText((Join-Path $root $name),'fixture data')
    }
    $files=@(Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
        @{path=$_.FullName.Substring($root.Length+1).Replace('\','/');sha256=(Hash-File $_.FullName)}
    })
    Atomic-Json (Join-Path $root 'payload.json') @{version=$Version;update_protocol=1;release_sequence=$Sequence;files=$files}
    $zip=Join-Path $testRoot ($Version+'.zip')
    [IO.Compression.ZipFile]::CreateFromDirectory($container,$zip)
    return [pscustomobject]@{zip=$zip;root=$root;update=[pscustomobject]@{protocol=1;min_launcher=1;version=$Version;sequence=$Sequence;tag=('test-'+$Sequence);package_sha256=(Hash-File $zip)}}
}
$a=Fixture 'TestBeta2' 2
$null=Install-VerifiedPackage $managed $a.zip $a.update
Assert ((Read-Json (Join-Path $managed 'active.json')).active.version -eq 'TestBeta2') 'Initial verified installation must activate.'
$stateHash=Hash-File (Join-Path $managed 'active.json')
$bad=$a.update.PSObject.Copy();$bad.package_sha256=('0'*64)
Reject { Install-VerifiedPackage $managed $a.zip $bad } 'Wrong download hash must be rejected.'
Assert ((Hash-File (Join-Path $managed 'active.json')) -eq $stateHash) 'Failed update must leave the active version unchanged.'
Write-Output 'PASS: Verified install; wrong download hash rejected without changing active version.'

$b=Fixture 'TestBeta3' 3;$null=Install-VerifiedPackage $managed $b.zip $b.update
$c=Fixture 'TestBeta4' 4;$null=Install-VerifiedPackage $managed $c.zip $c.update
$unmanaged=Join-Path $managed 'Versions\UserFiles';[IO.Directory]::CreateDirectory($unmanaged) | Out-Null
[IO.File]::WriteAllText((Join-Path $unmanaged 'personal.txt'),'do not delete')
$null=Clean-ManagedFiles $managed
Assert (!(Test-Path -LiteralPath (Version-Root $managed 'TestBeta2'))) 'Older managed versions must be cleaned.'
Assert (Test-Path -LiteralPath (Version-Root $managed 'TestBeta3')) 'Rollback version must be retained.'
Assert (Test-Path -LiteralPath (Version-Root $managed 'TestBeta4')) 'Active version must be retained.'
Assert (Test-Path -LiteralPath (Join-Path $unmanaged 'personal.txt')) 'Unmanaged user folder must be retained.'
Rollback-Version $managed
Assert ((Read-Json (Join-Path $managed 'active.json')).active.version -eq 'TestBeta3') 'Rollback must select previous version.'
Rollback-Version $managed
Assert ((Read-Json (Join-Path $managed 'active.json')).active.version -eq 'TestBeta4') 'Switching back must retain newer version.'
Assert ([IO.File]::ReadAllText($sentinel) -eq 'personal calibration and saves must survive') 'Cleanup must not change personal files.'
Assert (@(Get-ChildItem -LiteralPath $managed -Filter '*.previous').Count -le 1) 'State backups must remain bounded.'
Write-Output 'PASS: Only current and rollback versions retained; rollback works; personal/unmanaged files preserved.'

$tampered=Fixture 'TestTampered' 5
[IO.File]::WriteAllText((Join-Path $tampered.root 'a.txt'),'changed after manifest')
$tamperZip=Join-Path $testRoot 'tampered.zip'
[IO.Compression.ZipFile]::CreateFromDirectory((Split-Path -Parent $tampered.root),$tamperZip)
$tampered.update.package_sha256=Hash-File $tamperZip
$stateHash=Hash-File (Join-Path $managed 'active.json')
Reject { Install-VerifiedPackage $managed $tamperZip $tampered.update } 'Payload file corruption must be rejected.'
Assert ((Hash-File (Join-Path $managed 'active.json')) -eq $stateHash) 'Corrupt payload must not change activation.'

foreach ($paths in @(@('root/../../escape.txt'),@('root/A.txt','root/a.txt'))) {
    $zip=Join-Path $testRoot ([Guid]::NewGuid().ToString('N')+'.zip')
    $archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($path in $paths) {
            $entry=$archive.CreateEntry($path);$stream=$entry.Open()
            try { $stream.WriteByte(65) } finally { $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
    $target=Join-Path $testRoot ([Guid]::NewGuid().ToString('N'))
    Reject { Extract-CheckedZip $zip $target } 'Traversal or case-duplicate archive must be rejected.'
}
Reject { Remove-ManagedDirectory $managed 'Versions' $testRoot } 'Cleanup must reject paths outside managed buckets.'
Assert (!(Safe-Relative 'CON.txt')) 'Windows device filenames must be rejected.'
Assert (!(Safe-Relative 'root/../escape')) 'Parent paths must be rejected.'
Write-Output 'PASS: Corrupt payload, unsafe ZIP paths, duplicate paths, and out-of-scope cleanup rejected.'

$backupRoot=Join-Path $a.root 'ShortcutBackups'
[IO.Directory]::CreateDirectory($backupRoot) | Out-Null
$backupFile=Join-Path $backupRoot 'Barony VR - GA21 Beta.lnk.0123456789abcdef0123456789abcdef.bak'
$shortcut=New-Object byte[] 76
$header=[byte[]]@(76,0,0,0,1,20,2,0,0,0,0,0,192,0,0,0,0,0,0,70)
[Array]::Copy($header,$shortcut,20)
[IO.File]::WriteAllBytes($backupFile,$shortcut)
$manifestA=Read-Json (Join-Path $a.root 'payload.json')
Assert (Test-LegacyContents $a.root $manifestA) 'Valid installer-created shortcut backup must be recognized.'
$personal=Join-Path $a.root 'personal-document.txt';[IO.File]::WriteAllText($personal,'keep this')
Assert (!(Test-LegacyContents $a.root $manifestA)) 'An unexpected personal file must block legacy cleanup.'
[IO.File]::Delete($personal)
$shortcut[0]=0;[IO.File]::WriteAllBytes($backupFile,$shortcut)
Assert (!(Test-LegacyContents $a.root $manifestA)) 'Invalid shortcut contents must block legacy cleanup.'
Write-Output 'PASS: Owned shortcut backups recognized; unknown files and invalid shortcuts block cleanup.'

$originalHttp=(Get-Item Function:\Http-Json).ScriptBlock
$originalDownload=(Get-Item Function:\Download-ReleaseAsset).ScriptBlock
$descriptor=Join-Path $testRoot 'descriptor.json'
Atomic-Json $descriptor @{protocol=1;min_launcher=1;version='TestBeta5';sequence=5;tag='test-5';package_name='BaronyVR_TestBeta5_Windows.zip';package_sha256=('a'*64)}
$script:FixtureDescriptor=$descriptor
$script:FixtureFeed=@(
    [pscustomobject]@{draft=$true;tag_name='test-99';assets=@([pscustomobject]@{name='baronyvr-update.json'})},
    [pscustomobject]@{draft=$false;prerelease=$false;tag_name='test-5';name='Fixture release';html_url='https://github.com/Nyxaroth37/BaronyVR/releases/tag/test-5';assets=@(
        [pscustomobject]@{name='baronyvr-update.json'},
        [pscustomobject]@{name='BaronyVR_TestBeta5_Windows.zip';digest=('sha256:'+('a'*64))}
    )}
)
try {
    function Http-Json([string]$Uri) { Write-Output -NoEnumerate $script:FixtureFeed }
    function Download-ReleaseAsset($Asset,[string]$Destination,[long]$Limit=128MB) { Copy-Item -LiteralPath $script:FixtureDescriptor -Destination $Destination }
    $scratch=New-ManagedDirectory $managed 'Checks'
    $candidate=Find-Update 4 $scratch
    Assert ($candidate.update.sequence -eq 5) 'A newer compatible published release must be selected.'
    Assert ($null -eq (Find-Update 5 $scratch)) 'The current sequence must not be offered as an update.'
    $script:FixtureFeed[1].prerelease=$true
    Assert ($null -eq (Find-Update 4 $scratch)) 'Stable launcher must not offer prereleases.'
    $script:FixtureFeed[1].prerelease=$false
    $script:FixtureFeed[1].assets[1].digest='sha256:'+('b'*64)
    Assert ($null -eq (Find-Update 4 $scratch)) 'Descriptor and GitHub asset digests must agree.'
    Remove-ManagedDirectory $managed 'Checks' $scratch
} finally {
    Set-Item Function:\Http-Json $originalHttp
    Set-Item Function:\Download-ReleaseAsset $originalDownload
}
Write-Output 'PASS: Published update selection, sequence comparison, draft exclusion, and digest agreement.'

# Network lookup failure must not alter installed state.
$originalHttp=(Get-Item Function:\Http-Json).ScriptBlock
try {
    function Http-Json([string]$Uri) { throw 'Simulated offline connection' }
    $scratch=New-ManagedDirectory $managed 'Checks'
    Reject { Find-Update 4 $scratch } 'Offline feed lookup must report an error.'
    Remove-ManagedDirectory $managed 'Checks' $scratch
} finally { Set-Item Function:\Http-Json $originalHttp }
Assert ((Hash-File (Join-Path $managed 'active.json')) -eq $stateHash) 'Offline lookup must not alter installed state.'
for ($i=0;$i -lt 7;$i++) { $null=New-ManagedDirectory $managed 'Work' }
$null=Clean-ManagedFiles $managed
Assert (@(Get-ChildItem -LiteralPath (Join-Path $managed 'Work') -Directory).Count -le 3) 'Abandoned work must have bounded retention.'
Write-Output 'PASS: Offline lookup leaves installed game intact; abandoned work capped at three folders.'
$shortcutDirectory=Join-Path $testRoot 'ShortcutTest'
$shortcutLauncher=Join-Path $testRoot 'ShortcutLauncher'
[IO.Directory]::CreateDirectory($shortcutDirectory) | Out-Null
[IO.Directory]::CreateDirectory($shortcutLauncher) | Out-Null
$shortcutTarget=Join-Path $shortcutLauncher 'BaronyVR.exe'
[IO.File]::WriteAllText($shortcutTarget,'test fixture, never executed')
$oldShortcut=Join-Path $shortcutDirectory 'Barony VR - GA21 Beta.lnk'
$newShortcut=Join-Path $shortcutDirectory 'BaronyVR.lnk'
$shortcutShell=New-Object -ComObject WScript.Shell
$fixtureShortcut=$shortcutShell.CreateShortcut($oldShortcut)
$fixtureShortcut.TargetPath=$shortcutTarget;$fixtureShortcut.Save()
Install-LauncherShortcut $shortcutDirectory $managed $shortcutLauncher
Assert (Test-Path -LiteralPath $newShortcut) 'The public BaronyVR shortcut must be created.'
Assert (!(Test-Path -LiteralPath $oldShortcut)) 'The recognized beta shortcut must be removed after migration.'
Assert ($shortcutShell.CreateShortcut($newShortcut).TargetPath -eq $shortcutTarget) 'The public shortcut must target BaronyVR.exe.'
$shortcutHash=Hash-File $newShortcut
$unrelatedShortcut=$shortcutShell.CreateShortcut($oldShortcut)
$unrelatedShortcut.TargetPath=Join-Path $env:WINDIR 'System32\notepad.exe';$unrelatedShortcut.Save()
Reject { Install-LauncherShortcut $shortcutDirectory $managed $shortcutLauncher } 'An unrelated shortcut must block migration.'
Assert ((Hash-File $newShortcut) -eq $shortcutHash) 'Unrelated shortcut rejection must not change the valid public shortcut.'
Assert (Test-Path -LiteralPath $oldShortcut) 'An unrelated old shortcut must be preserved.'
Write-Output 'PASS: Public shortcut migration, duplicate beta shortcut removal, and unrelated-shortcut protection.'
Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'Bootstrap\UiLoop.ps1')
$script:HiddenTicks=0
$window=New-Object Windows.Forms.Form
$window.ShowInTaskbar=$false;$window.Opacity=0
$uiTimer=New-Object Windows.Forms.Timer;$uiTimer.Interval=75
$uiTimer.Add_Tick({
    if (!$window.Visible) { $script:HiddenTicks++ }
    if ($script:HiddenTicks -ge 3) { $uiTimer.Stop();$window.Close() }
})
$window.Add_Shown({$window.Hide();$uiTimer.Start()})
try { Run-LauncherLoop $window } finally { $uiTimer.Dispose();$window.Dispose() }
Assert ($script:HiddenTicks -ge 3) 'The launcher message loop must remain alive after its window hides.'
Write-Output 'PASS: Launcher stays alive while hidden and processes completion timers.'
Write-Output "ALL UPDATER TESTS PASSED. Fixtures: $testRoot"
