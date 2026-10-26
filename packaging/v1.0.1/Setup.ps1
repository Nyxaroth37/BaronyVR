Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$ErrorActionPreference='Stop'
try {
    . (Join-Path $PSScriptRoot 'Bootstrap\UpdateCore.ps1')
    $null=Verify-Payload $PSScriptRoot
    . (Join-Path $PSScriptRoot 'Common.ps1')
    Check-Prerequisites
} catch {
    [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Barony VR setup','OK','Error') | Out-Null;exit 1
}
$managedRoot=Join-Path $env:LOCALAPPDATA 'BaronyVR\Managed'
$launcherRoot=Join-Path $managedRoot 'Launcher'
$form=New-Object Windows.Forms.Form;$form.Text='BaronyVR 1.0.1 | Setup'
$form.ClientSize=New-Object Drawing.Size(660,465);$form.StartPosition='CenterScreen';$form.FormBorderStyle='FixedDialog';$form.MaximizeBox=$false
$form.Font=New-Object Drawing.Font('Segoe UI',10);$form.BackColor=[Drawing.Color]::FromArgb(27,30,36);$form.ForeColor=[Drawing.Color]::WhiteSmoke
function Setup-Label([string]$Text,[int]$Y,[int]$Height) {
    $label=New-Object Windows.Forms.Label;$label.Text=$Text;$label.Location=New-Object Drawing.Point(28,$Y);$label.Size=New-Object Drawing.Size(604,$Height);$form.Controls.Add($label);return $label
}
$title=Setup-Label 'BARONY VR' 24 45;$title.Font=New-Object Drawing.Font('Segoe UI',23,[Drawing.FontStyle]::Bold)
$null=Setup-Label 'VERSION 1.0.1  /  CALIBRATION & BOTTLE INTERACTIONS' 78 28
$null=Setup-Label "Native calibration tabs, labeled sliders and grip-to-drink bottles.`r`nVerified updates, preserved settings and one-version rollback." 119 52
$null=Setup-Label 'Steam Barony folder (verified build 22630456)' 189 27
$pathBox=New-Object Windows.Forms.TextBox;$pathBox.Text=Find-Retail;$pathBox.Location=New-Object Drawing.Point(28,221);$pathBox.Size=New-Object Drawing.Size(485,28);$form.Controls.Add($pathBox)
$browse=New-Object Windows.Forms.Button;$browse.Text='Browse...';$browse.Location=New-Object Drawing.Point(527,219);$browse.Size=New-Object Drawing.Size(105,31);$form.Controls.Add($browse)
$browse.Add_Click({$dialog=New-Object Windows.Forms.FolderBrowserDialog;$dialog.ShowNewFolderButton=$false;if($dialog.ShowDialog()-eq'OK'){$pathBox.Text=$dialog.SelectedPath};$dialog.Dispose()})
$null=Setup-Label "Existing calibration and saves are preserved. Missing settings use ResidentMonster's defaults.`r`nManaged updates retain the active and previous version and clean temporary files.`r`nThe verified older Beta 1 folder is cleaned after the first successful game exit." 273 75
$status=Setup-Label 'Ready to install BaronyVR 1.0.1. Existing settings will be preserved.' 364 28
$install=New-Object Windows.Forms.Button;$install.Text='Install launcher';$install.Location=New-Object Drawing.Point(439,412);$install.Size=New-Object Drawing.Size(193,36);$form.Controls.Add($install)
function Copy-Payload([string]$From,[string]$To,[switch]$Legacy) {
    $manifest=Verify-Payload $From -Legacy:$Legacy
    [IO.Directory]::CreateDirectory($To) | Out-Null
    foreach ($entry in $manifest.files) {
        $destination=Join-Path $To $entry.path
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $From $entry.path) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $From 'payload.json') -Destination (Join-Path $To 'payload.json')
    $null=Verify-Payload $To -Legacy:$Legacy
}
$install.Add_Click({
    if ($script:SetupDone) { Start-Process (Join-Path $launcherRoot 'BaronyVR.exe');$form.Close();return }
    $install.Enabled=$false
    try {
        if (Get-Process -Name barony -ErrorAction SilentlyContinue) { throw 'Close Barony before installing.' }
        if (Get-Process -Name BaronyVR -ErrorAction SilentlyContinue) { throw 'Close the existing Barony VR launcher before installing.' }
        $retail=[IO.Path]::GetFullPath($pathBox.Text.Trim()).TrimEnd('\');Check-Retail $retail
        [IO.Directory]::CreateDirectory($managedRoot) | Out-Null
        $configurationPath=Join-Path $managedRoot 'configuration.json'
        if (Test-Path -LiteralPath $configurationPath) {
            if ((Read-Json $configurationPath).retail -ne $retail) { throw 'The launcher is already configured for a different retail folder; it was left unchanged.' }
        } else { Atomic-Json $configurationPath @{retail=[string]$retail} }
        $status.Text='Verifying and staging the versioned installation...';$form.Refresh()
        $statePath=Join-Path $managedRoot 'active.json'
        if (Test-Path -LiteralPath $statePath) {
            if ((Read-Json $statePath).active.sequence -gt 4) { throw 'A newer managed version is installed. Use the launcher instead of downgrading.' }
        }
        $versionRoot=Version-Root $managedRoot 'BaronyVR-1.0.1'
        if (!(Test-Path -LiteralPath $versionRoot)) {
            $stage=New-ManagedDirectory $managedRoot 'Staging'
            $candidate=Join-Path $stage 'Version';Copy-Payload $PSScriptRoot $candidate
            Atomic-Json (Join-Path $candidate 'installation.json') @{retail=[string]$retail}
            [IO.Directory]::CreateDirectory((Split-Path -Parent $versionRoot)) | Out-Null
            [IO.Directory]::Move($candidate,$versionRoot);Mark-Managed $managedRoot $versionRoot 'Versions'
            Remove-ManagedDirectory $managedRoot 'Staging' $stage
        } else {
            $null=Verify-Payload $versionRoot
            if ((Hash-File (Join-Path $versionRoot 'payload.json')) -ne (Hash-File (Join-Path $PSScriptRoot 'payload.json'))) { throw 'Installed 1.0.1 differs from this package. It was preserved.' }
        }
        # Import the exact known Beta 1 as the initial rollback version.
        $legacy=Join-Path $env:LOCALAPPDATA 'BaronyVR\GA21-PublicBeta1'
        if (!(Test-Path -LiteralPath $statePath) -and (Test-Path -LiteralPath (Join-Path $legacy 'payload.json'))) {
            if ((Hash-File (Join-Path $legacy 'payload.json')) -ne '11B7506E973134EDA161E8F5658469050806EAFFD61D21C0B430617031357E40') { throw 'Existing Beta 1 manifest differs; migration stopped without replacing it.' }
            $null=Verify-Payload $legacy -Legacy
            if ((Read-Json (Join-Path $legacy 'installation.json')).retail -ne $retail) { throw 'Existing Beta 1 uses a different retail folder.' }
            $previousRoot=Version-Root $managedRoot 'GA21-PublicBeta1'
            if (!(Test-Path -LiteralPath $previousRoot)) {
                $stage=New-ManagedDirectory $managedRoot 'Staging'
                $candidate=Join-Path $stage 'Legacy';Copy-Payload $legacy $candidate -Legacy
                Atomic-Json (Join-Path $candidate 'installation.json') @{retail=[string]$retail}
                [IO.Directory]::Move($candidate,$previousRoot);Mark-Managed $managedRoot $previousRoot 'Versions'
                Remove-ManagedDirectory $managedRoot 'Staging' $stage
            }
            Activate-Version $managedRoot (Make-Record $previousRoot 1 'ga21-public-beta1')
            if (!(Test-Path -LiteralPath (Join-Path $legacy '.baronyvr-owned.json'))) { Mark-Managed $managedRoot $legacy 'Legacy' }
        }
        $bootstrapSource=Join-Path $PSScriptRoot 'Bootstrap'
        Verify-Bootstrap $bootstrapSource
        if (Test-Path -LiteralPath $launcherRoot) {
            Verify-Bootstrap $launcherRoot
            $oldHash=Hash-File (Join-Path $launcherRoot 'bootstrap-manifest.json')
            $newHash=Hash-File (Join-Path $bootstrapSource 'bootstrap-manifest.json')
            if ($oldHash -ne $newHash) {
                $known=@('20768979A90B76F0369BA02E7DED266B723F02444F2204709928A0922B02661F','B6E778497429662C266BE4055CB2B54A8FBDB615920A8E9EF18677A7E6A69935','B4E41C7623CA3C386952F58C0A7944E05A8921027B3BDC54103536A7DD195529')
                if ($oldHash -notin $known) { throw 'A different launcher candidate already exists; it was preserved.' }
                $previousLauncher=Join-Path $managedRoot 'Launcher.previous'
                if (Test-Path -LiteralPath $previousLauncher) { $null=Clean-PreviousLauncher $managedRoot }
                $stage=New-ManagedDirectory $managedRoot 'Staging'
                $candidateLauncher=Join-Path $stage 'Launcher'
                Copy-Item -LiteralPath $bootstrapSource -Destination $candidateLauncher -Recurse
                Verify-Bootstrap $candidateLauncher
                [IO.Directory]::Move($launcherRoot,$previousLauncher)
                try { [IO.Directory]::Move($candidateLauncher,$launcherRoot) }
                catch {
                    if (!(Test-Path -LiteralPath $launcherRoot)) { [IO.Directory]::Move($previousLauncher,$launcherRoot) }
                    throw
                }
                Remove-ManagedDirectory $managedRoot 'Staging' $stage
            }
        } else {
            Copy-Item -LiteralPath $bootstrapSource -Destination $launcherRoot -Recurse
        }
        $null=Seed-Defaults $versionRoot $retail
        Activate-Version $managedRoot (Make-Record $versionRoot 4 'v1.0.1')
        foreach ($directory in @([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('Programs'))) {
            Install-LauncherShortcut $directory $managedRoot $launcherRoot
        }
        $null=Clean-ManagedFiles $managedRoot
        $script:SetupDone=$true;$pathBox.Enabled=$false;$browse.Enabled=$false;$install.Text='Open launcher'
        $status.Text='Installed. The desktop shortcut now opens the update-enabled launcher.'
    } catch { $status.Text='Setup stopped; existing files were preserved where verification failed.';[Windows.Forms.MessageBox]::Show($_.Exception.Message,'Barony VR setup','OK','Error') | Out-Null }
    finally { $install.Enabled=$true }
})
$form.AcceptButton=$install
[void]$form.ShowDialog();$form.Dispose()
