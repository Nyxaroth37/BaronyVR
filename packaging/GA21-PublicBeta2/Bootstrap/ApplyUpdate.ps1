param([string]$ManagedRoot, [string]$RequestFile, [string]$OutputFile)
try {
    . (Join-Path $PSScriptRoot 'UpdateCore.ps1')
    $request=Read-Json $RequestFile
    # Re-read the official feed instead of trusting a stale UI selection.
    $scratch=New-ManagedDirectory $ManagedRoot 'Checks'
    $state=Read-Json (Join-Path $ManagedRoot 'active.json')
    $current=Find-Update ([int]$state.active.sequence) $scratch
    if (!$current -or $current.update.tag -cne $request.tag -or $current.update.package_sha256 -ne $request.package_sha256) {
        throw 'The selected release changed or is no longer available. Check for updates again.'
    }
    $zip=Join-Path $scratch ([string]$current.update.package_name)
    Download-ReleaseAsset $current.asset $zip
    $record=Install-VerifiedPackage $ManagedRoot $zip $current.update
    Atomic-Json $OutputFile @{ok=$true;version=[string]$record.version}
    [IO.File]::Delete($zip)
} catch {
    @{ok=$false;error=[string]$_.Exception.Message} | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath $OutputFile -Encoding UTF8
    exit 1
} finally { if ($scratch -and (Test-Path -LiteralPath $scratch)) { try { Remove-ManagedDirectory $ManagedRoot 'Checks' $scratch } catch {} } }
