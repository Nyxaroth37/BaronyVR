try {
    . (Join-Path $PSScriptRoot 'Common.ps1')
    Check-Prerequisites
    $null = Check-Payload $PSScriptRoot
    $settings = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installation.json') -Raw | ConvertFrom-Json
    $retail = [string]$settings.retail
    Check-Retail $retail
    Check-XrRuntime
    if (Get-Process -Name barony -ErrorAction SilentlyContinue) { throw 'Barony is already running. Close it before starting Barony VR.' }
    $null = Seed-Defaults $PSScriptRoot $retail
    $launch = New-Object System.Diagnostics.ProcessStartInfo
    $launch.FileName = Join-Path $PSScriptRoot 'Runtime\barony.exe'
    $launch.WorkingDirectory = $retail
    $launch.UseShellExecute = $false
    $launch.CreateNoWindow = $true
    $launch.EnvironmentVariables['PATH'] = "$retail;$env:PATH"
    $launch.EnvironmentVariables['SteamAppId'] = '371970'
    $launch.EnvironmentVariables['SteamGameId'] = '371970'
    $game = [System.Diagnostics.Process]::Start($launch)
    $game.WaitForExit()
    if ($game.ExitCode -ne 0) { throw "Barony VR exited with code $($game.ExitCode). The game log and crashlogs are in: $retail" }
} catch {
    Add-Type -AssemblyName System.Windows.Forms
    [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Barony VR could not start', 'OK', 'Error') | Out-Null
    exit 1
}
