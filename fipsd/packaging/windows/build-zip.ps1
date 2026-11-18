# Build the Windows ZIP package for fipsd (upstream packaging/windows/build-zip.ps1).
#
# Usage: powershell -File fipsd/packaging/windows/build-zip.ps1 -BuildDir <cmake build dir>
#          [-Version <version>] [-Arch arm64|amd64] [-DllPath <dir>,<dir>]
#          [-WintunZip <wintun-0.14.1.zip>] [-OutDir <dir>]
# Output: <OutDir>\fipsd-<version>-windows-<arch>.zip (default OutDir: <BuildDir>\deploy)
#
# Prerequisites: fipsd and fipsctl (and fipstop, when ncurses was found) built
# in BuildDir as fipsd/README.md "Building on Windows (MSYS2)" describes, and
# llvm-objdump or objdump (MSYS2's clang toolchain) on PATH or in -DllPath.
# Unlike upstream's, this script does not build: the MSYS2 build is its input.
#
# The ZIP is flat: the executables, every non-system DLL they load (resolved
# from their import tables through the build tree, -DllPath and PATH), the
# Wintun DLL for -Arch fetched from wintun.net and checked against the pinned
# SHA-256 and its WireGuard LLC signature, the Windows example fips.yaml, the
# hosts file, the service scripts and README.txt.

param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$Version = "",
    [ValidateSet("", "arm64", "amd64")][string]$Arch = "",
    [string[]]$DllPath = @(),
    [string]$WintunZip = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PackagingDir = Split-Path -Parent $ScriptDir
$FipsdDir = Split-Path -Parent $PackagingDir
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path

# The pinned Wintun release (upstream fips CI pins the same zip).
$WintunUrl = "https://www.wintun.net/builds/wintun-0.14.1.zip"
$WintunSha256 = "07C256185D6EE3652E09FA55C0B673E2624B565E02C4B9091C79CA7D2F24EF51"

# The component version, from declare_component_version(FIPSD <major> <minor> <patch>).
if (-not $Version) {
    $cmake = Get-Content -LiteralPath "$FipsdDir\CMakeLists.txt" -Raw
    if ($cmake -match 'declare_component_version\(FIPSD\s+(\d+)\s+(\d+)\s+(\d+)\)') {
        $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
    } else {
        Write-Error "Could not determine the fipsd version from $FipsdDir\CMakeLists.txt"
        exit 1
    }
}
if (-not $Arch) {
    $Arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "amd64" }
}
if (-not $OutDir) { $OutDir = Join-Path $BuildDir "deploy" }

$PkgName = "fipsd-$Version-windows-$Arch"
$BinaryDir = Join-Path $BuildDir "fipsd"
$StagingDir = Join-Path $env:TEMP "fipsd-staging-$([guid]::NewGuid().ToString('N'))"

Write-Host "Packaging fipsd v$Version for Windows $Arch from $BinaryDir..."

$Binaries = @("fipsd.exe", "fipsctl.exe")
foreach ($bin in $Binaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryDir $bin))) {
        Write-Error "Missing binary: $BinaryDir\$bin"
        exit 1
    }
}
if (Test-Path -LiteralPath (Join-Path $BinaryDir "fipstop.exe")) {
    $Binaries += "fipstop.exe"
} else {
    Write-Warning "fipstop.exe was not built (no ncurses); the package ships without it"
}

# An import-table reader: llvm-objdump or GNU objdump prints "DLL Name: <name>".
$objdump = $null
foreach ($name in @("llvm-objdump.exe", "objdump.exe")) {
    foreach ($dir in $DllPath) {
        $candidate = Join-Path $dir $name
        if (Test-Path -LiteralPath $candidate) { $objdump = $candidate; break }
    }
    if ($objdump) { break }
    $found = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $objdump = $found.Source; break }
}
if (-not $objdump) {
    Write-Error "Neither llvm-objdump nor objdump was found on PATH or in -DllPath"
    exit 1
}

# Where a DLL may come from: the build tree (shared libraries it built), then
# -DllPath, then PATH. Nothing under the Windows directory is ever shipped.
$systemRoot = [IO.Path]::GetFullPath($env:SystemRoot).TrimEnd('\') + '\'
$searchDirs = New-Object System.Collections.Generic.List[string]
foreach ($dll in @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter "*.dll" -File -ErrorAction SilentlyContinue)) {
    if (-not $searchDirs.Contains($dll.DirectoryName)) { $searchDirs.Add($dll.DirectoryName) }
}
foreach ($dir in @($DllPath) + @($env:PATH -split ';')) {
    if (-not $dir) { continue }
    try { $full = [IO.Path]::GetFullPath($dir) } catch { continue }
    if ($full.TrimEnd('\') + '\' -like "$systemRoot*") { continue }
    if ((Test-Path -LiteralPath $full -PathType Container) -and -not $searchDirs.Contains($full)) { $searchDirs.Add($full) }
}

function Get-Import([string]$Path) {
    $lines = & $objdump -p $Path 2>$null
    if ($LASTEXITCODE -ne 0) { throw "$objdump could not read $Path" }
    foreach ($line in $lines) {
        if ($line -match '^\s*DLL Name:\s*(\S+)\s*$') { $Matches[1] }
    }
}

# The import closure of the executables, by name (Windows matches DLL names
# case-insensitively).
$shipped = @{}
$queue = New-Object System.Collections.Generic.Queue[string]
foreach ($bin in $Binaries) { $queue.Enqueue((Join-Path $BinaryDir $bin)) }
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    foreach ($import in @(Get-Import $file)) {
        $key = $import.ToLowerInvariant()
        if ($shipped.ContainsKey($key)) { continue }
        if ($key.StartsWith("api-ms-win-") -or $key.StartsWith("ext-ms-win-")) { continue }
        if (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$import")) { continue }
        $source = $null
        foreach ($dir in $searchDirs) {
            $candidate = Join-Path $dir $import
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $source = $candidate; break }
        }
        if (-not $source) {
            Write-Error "$file imports $import, which is neither a system DLL nor found in the build tree, -DllPath or PATH"
            exit 1
        }
        $shipped[$key] = $source
        $queue.Enqueue($source)
    }
}

New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null
try {
    foreach ($bin in $Binaries) { Copy-Item -LiteralPath (Join-Path $BinaryDir $bin) -Destination $StagingDir }
    foreach ($source in $shipped.Values) { Copy-Item -LiteralPath $source -Destination $StagingDir }
    Write-Host "  $($Binaries.Count) executables, $($shipped.Count) runtime DLLs"

    # Wintun: the pinned zip, its hash, and the signed DLL for this architecture.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $wintunWork = Join-Path $StagingDir ".wintun"
    New-Item -ItemType Directory -Force -Path $wintunWork | Out-Null
    if (-not $WintunZip) {
        $WintunZip = Join-Path $wintunWork "wintun-0.14.1.zip"
        Write-Host "  Fetching $WintunUrl"
        Invoke-WebRequest -UseBasicParsing -Uri $WintunUrl -OutFile $WintunZip
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $WintunZip).Hash
    if ($hash -ne $WintunSha256) {
        Write-Error "$WintunZip has SHA-256 $hash, not the pinned $WintunSha256"
        exit 1
    }
    Expand-Archive -LiteralPath $WintunZip -DestinationPath $wintunWork
    $wintunDll = Join-Path $wintunWork "wintun\bin\$Arch\wintun.dll"
    $signature = Get-AuthenticodeSignature -LiteralPath $wintunDll
    if ($signature.Status -ne "Valid" -or $signature.SignerCertificate.Subject -notmatch 'CN=WireGuard LLC') {
        Write-Error "$wintunDll is not validly signed by WireGuard LLC ($($signature.Status): $($signature.StatusMessage))"
        exit 1
    }
    Copy-Item -LiteralPath $wintunDll -Destination $StagingDir
    Copy-Item -LiteralPath (Join-Path $wintunWork "wintun\LICENSE.txt") -Destination (Join-Path $StagingDir "wintun-LICENSE.txt")
    Remove-Item -Recurse -Force -LiteralPath $wintunWork
    Write-Host "  wintun.dll ($Arch) from wintun-0.14.1.zip, SHA-256 verified, signed by WireGuard LLC"

    Copy-Item -LiteralPath (Join-Path $ScriptDir "fips.yaml") -Destination (Join-Path $StagingDir "fips.yaml")
    Copy-Item -LiteralPath (Join-Path $PackagingDir "common\hosts") -Destination (Join-Path $StagingDir "hosts")
    Copy-Item -LiteralPath (Join-Path $ScriptDir "install-service.ps1") -Destination $StagingDir
    Copy-Item -LiteralPath (Join-Path $ScriptDir "uninstall-service.ps1") -Destination $StagingDir

    @"
fipsd v$Version for Windows ($Arch)
=====================================

Contents:
  fipsd.exe, fipsctl.exe$(if ($Binaries -contains "fipstop.exe") { ", fipstop.exe" }), the runtime DLLs they load,
  wintun.dll (Wintun 0.14.1 by WireGuard LLC; wintun-LICENSE.txt),
  fips.yaml (example), hosts, install-service.ps1, uninstall-service.ps1.

Quick start (foreground, elevated prompt for the TUN adapter):
  .\fipsd.exe -c fips.yaml

Windows service (Administrator):
  # Install: binaries to C:\Program Files\fips, the service 'fipsd'
  powershell -ExecutionPolicy Bypass -File install-service.ps1
  sc.exe start fipsd

  # Upgrade: stop the service first, then rerun the installer
  sc.exe stop fipsd
  powershell -ExecutionPolicy Bypass -File install-service.ps1
  sc.exe start fipsd

  # Manage
  sc.exe query fipsd
  sc.exe stop fipsd

  # Uninstall (add -RemoveAll to delete C:\ProgramData\fips too)
  powershell -ExecutionPolicy Bypass -File uninstall-service.ps1

Control socket:
  TCP 127.0.0.1:21210. fipsctl and fipstop connect there by default
  (-s <port> for another). It accepts loopback connections only, but has
  no access control beyond that: any local user can query and command the
  daemon, as with upstream FIPS on Windows. The native datagram API is not
  available on Windows.

Configuration:
  The service reads C:\ProgramData\fips\fips.yaml (install-service.ps1
  seeds it from fips.yaml and sets FIPS_CONFIG to it) and the hosts,
  peers.allow and peers.deny files beside it. The installer restricts
  C:\ProgramData\fips to SYSTEM and Administrators, so editing those files
  needs an elevated prompt. Empty peers.allow and peers.deny allow every
  peer; empty them to clear a list, do not delete them.

Firewall:
  The installer adds no firewall rule (the control port is loopback-only).
  Windows treats the fips0 adapter as a Public network: allow inbound
  traffic you want from the mesh (for example ICMPv6 echo on fips0), and
  the UDP transport port (2121) if peers must reach this node first.

Logs:
  The service logs to C:\ProgramData\fips\fips.log, rolled at 10 MiB with
  four old files kept (node.log_max_size_mb, node.log_max_files). A
  foreground run logs to the console.
"@ | Out-File -FilePath (Join-Path $StagingDir "README.txt") -Encoding ASCII

    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $ZipPath = Join-Path $OutDir "$PkgName.zip"
    if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath }
    Compress-Archive -Path (Join-Path $StagingDir "*") -DestinationPath $ZipPath
} finally {
    Remove-Item -Recurse -Force -LiteralPath $StagingDir -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Package built: $ZipPath"
Write-Host "  Size: $([math]::Round((Get-Item -LiteralPath $ZipPath).Length / 1MB, 2)) MB"
Write-Host "  SHA-256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $ZipPath).Hash)"
