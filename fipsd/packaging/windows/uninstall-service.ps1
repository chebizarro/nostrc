# Uninstall the fipsd Windows service (upstream packaging/windows/uninstall-service.ps1).
#
# Usage: powershell -ExecutionPolicy Bypass -File uninstall-service.ps1 [-RemoveAll]
# Requires: Administrator privileges
#
# By default preserves config in %ProgramData%\fips\.
# Pass -RemoveAll to remove config and logs too.

param(
    [switch]$RemoveAll
)

$ErrorActionPreference = "Stop"

# Check for admin
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "This script requires Administrator privileges. Right-click PowerShell and select 'Run as Administrator'."
    exit 1
}

$ServiceName = "fipsd"
$InstallDir = "$env:ProgramFiles\fips"
$ConfigDir = "$env:ProgramData\fips"

Write-Host "Uninstalling the fipsd service..."

# Stop the service if running. Stop-Service waits for STOPPED; the daemon
# removes its Wintun adapter, address and route as it stops.
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne "Stopped") {
    Write-Host "  Stopping service..."
    Stop-Service -Name $ServiceName -Force
}

# Remove the registration: through the installed fipsd.exe, as upstream, or
# with sc.exe when the binary is already gone.
if ($svc) {
    Write-Host "  Removing service registration..."
    if (Test-Path -LiteralPath "$InstallDir\fipsd.exe") {
        & "$InstallDir\fipsd.exe" --uninstall-service
    } else {
        & "$env:SystemRoot\System32\sc.exe" delete $ServiceName
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Could not remove the $ServiceName service (exit code $LASTEXITCODE)"
        exit 1
    }
}

# Remove binaries
if (Test-Path -LiteralPath $InstallDir) {
    Write-Host "  Removing $InstallDir"
    Remove-Item -Recurse -Force -LiteralPath $InstallDir
}

# Remove from PATH
$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
if (($machinePath -split ";") -contains $InstallDir) {
    $newPath = ($machinePath -split ";" | Where-Object { $_ -ne $InstallDir }) -join ";"
    [Environment]::SetEnvironmentVariable("Path", $newPath, "Machine")
    Write-Host "  Removed from system PATH"
}

# Remove FIPS_CONFIG env var
[Environment]::SetEnvironmentVariable("FIPS_CONFIG", $null, "Machine")

# Config removal
if ($RemoveAll) {
    if (Test-Path -LiteralPath $ConfigDir) {
        Write-Host "  Removing config and logs at $ConfigDir"
        Remove-Item -Recurse -Force -LiteralPath $ConfigDir
    }
} else {
    if (Test-Path -LiteralPath $ConfigDir) {
        Write-Host "  Preserving config at $ConfigDir"
    }
}

Write-Host ""
Write-Host "fipsd service uninstalled."
if (-not $RemoveAll) {
    Write-Host "Config preserved at $ConfigDir (use -RemoveAll to delete)"
}
