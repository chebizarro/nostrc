# =============================================================================
# bundle-deps.ps1 - Bundle GTK4/libadwaita dependencies for Windows installer
# =============================================================================
#
# PowerShell script to collect all DLL dependencies for GNostr Signer.
# Can be used with MSYS2 MinGW or vcpkg installations.
#
# Usage:
#   .\bundle-deps.ps1 -BuildDir <path> [-OutputDir <path>] [-MsysPrefix <path>]
#
# Parameters:
#   -BuildDir     Path to build directory containing executables (required)
#   -OutputDir    Output directory for bundled deps (default: .\deps)
#   -MsysPrefix   MSYS2 MinGW prefix (default: C:\msys64\mingw64)
#   -VcpkgRoot    vcpkg installed directory (alternative to MSYS2)
#   -Verbose      Show detailed output
#
# =============================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$BuildDir,

    [string]$OutputDir = ".\deps",

    [string]$MsysPrefix = "C:\msys64\mingw64",

    [string]$VcpkgRoot = "",

    [switch]$UseVcpkg
)

$ErrorActionPreference = "Stop"

# Colors for output
function Write-Log {
    param([string]$Message, [string]$Color = "Green")
    Write-Host "[BUNDLE] " -ForegroundColor $Color -NoNewline
    Write-Host $Message
}

function Write-Warn {
    param([string]$Message)
    Write-Host "[WARN] " -ForegroundColor Yellow -NoNewline
    Write-Host $Message
}

function Write-Err {
    param([string]$Message)
    Write-Host "[ERROR] " -ForegroundColor Red -NoNewline
    Write-Host $Message
    exit 1
}

# Determine dependency source
if ($UseVcpkg -or $VcpkgRoot) {
    if (-not $VcpkgRoot) {
        $VcpkgRoot = "$env:VCPKG_ROOT\installed\x64-windows"
    }
    if (-not (Test-Path $VcpkgRoot)) {
        Write-Err "vcpkg root not found: $VcpkgRoot"
    }
    $DepsRoot = $VcpkgRoot
    $BinDir = "$DepsRoot\bin"
    Write-Log "Using vcpkg: $VcpkgRoot"
} else {
    if (-not (Test-Path $MsysPrefix)) {
        Write-Err "MSYS2 prefix not found: $MsysPrefix"
    }
    $DepsRoot = $MsysPrefix
    $BinDir = "$DepsRoot\bin"
    Write-Log "Using MSYS2: $MsysPrefix"
}

# Validate build directory
if (-not (Test-Path $BuildDir)) {
    Write-Err "Build directory not found: $BuildDir"
}

$ExePath = Join-Path $BuildDir "apps\gnostr-signer\gnostr-signer.exe"
$DaemonPath = Join-Path $BuildDir "apps\gnostr-signer\gnostr-signer-daemon.exe"

if (-not (Test-Path $ExePath)) {
    Write-Err "Executable not found: $ExePath"
}

# Create output directories
Write-Log "Creating output directories..."
$Directories = @(
    "$OutputDir\bin",
    "$OutputDir\lib\gdk-pixbuf-2.0\2.10.0\loaders",
    "$OutputDir\lib\gtk-4.0\4.0.0\media",
    "$OutputDir\lib\gtk-4.0\4.0.0\printbackends",
    "$OutputDir\share\glib-2.0\schemas",
    "$OutputDir\share\gtk-4.0",
    "$OutputDir\share\icons\Adwaita",
    "$OutputDir\share\icons\hicolor",
    "$OutputDir\share\locale",
    "$OutputDir\share\libadwaita-1"
)

foreach ($dir in $Directories) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

# -----------------------------------------------------------------------------
# DLL Dependency Collection
# -----------------------------------------------------------------------------

$CollectedDlls = @{}
$SystemDllPatterns = @(
    "^kernel32", "^user32", "^gdi32", "^shell32", "^ole32", "^oleaut32",
    "^advapi32", "^ws2_32", "^msvcrt", "^ntdll", "^comctl32", "^comdlg32",
    "^shlwapi", "^version", "^secur32", "^crypt32", "^bcrypt", "^ncrypt",
    "^rpcrt4", "^imm32", "^usp10", "^dwrite", "^d2d1", "^d3d", "^dwmapi",
    "^uxtheme", "^msimg32", "^winmm", "^winspool", "^wldap32",
    "^api-ms-", "^ext-ms-", "^ucrtbase", "^vcruntime", "^msvcp"
)

function Get-DllDependencies {
    param([string]$BinaryPath)

    $deps = @()

    try {
        # Use dumpbin if available (Visual Studio), otherwise use objdump
        $dumpbin = Get-Command "dumpbin" -ErrorAction SilentlyContinue
        $objdump = Get-Command "objdump" -ErrorAction SilentlyContinue

        if ($dumpbin) {
            $output = & dumpbin /DEPENDENTS $BinaryPath 2>$null
            $deps = $output | Where-Object { $_ -match "^\s+\S+\.dll$" } |
                    ForEach-Object { $_.Trim() }
        } elseif ($objdump) {
            $output = & objdump -p $BinaryPath 2>$null
            $deps = $output | Where-Object { $_ -match "DLL Name:" } |
                    ForEach-Object { ($_ -split "DLL Name:\s*")[1].Trim() }
        } else {
            # Fallback: use PowerShell PE parser
            $pe = [System.Reflection.Assembly]::LoadFile($BinaryPath)
            Write-Warn "No objdump/dumpbin found. Using limited PE analysis."
        }
    } catch {
        Write-Warn "Could not analyze: $BinaryPath"
    }

    return $deps
}

function Collect-Dlls {
    param([string]$BinaryPath)

    $binaryName = Split-Path -Leaf $BinaryPath
    Write-Verbose "Analyzing: $binaryName"

    $dlls = Get-DllDependencies -BinaryPath $BinaryPath

    foreach ($dll in $dlls) {
        $dllLower = $dll.ToLower()

        # Skip if already collected
        if ($CollectedDlls.ContainsKey($dllLower)) {
            continue
        }

        # Skip system DLLs
        $isSystem = $false
        foreach ($pattern in $SystemDllPatterns) {
            if ($dllLower -match $pattern) {
                $isSystem = $true
                break
            }
        }
        if ($isSystem) {
            Write-Verbose "  Skipping system DLL: $dll"
            continue
        }

        # Find the DLL
        $dllPath = Join-Path $BinDir $dll
        if (Test-Path $dllPath) {
            Write-Log "  Collecting: $dll"
            $CollectedDlls[$dllLower] = $dllPath
            Copy-Item $dllPath -Destination "$OutputDir\bin\" -Force

            # Recursively collect dependencies
            Collect-Dlls -BinaryPath $dllPath
        } else {
            Write-Warn "  DLL not found: $dll"
        }
    }
}

Write-Log "Collecting DLL dependencies..."
Collect-Dlls -BinaryPath $ExePath

if (Test-Path $DaemonPath) {
    Collect-Dlls -BinaryPath $DaemonPath
}

Write-Log "Collected $($CollectedDlls.Count) DLLs"

# -----------------------------------------------------------------------------
# Copy GLib utilities
# -----------------------------------------------------------------------------

Write-Log "Copying GLib utilities..."

$GlibBins = @(
    "glib-compile-schemas.exe",
    "gspawn-win64-helper.exe",
    "gspawn-win64-helper-console.exe",
    "gdbus.exe",
    "gio.exe",
    "gresource.exe",
    "gtk4-update-icon-cache.exe",
    "gtk4-query-settings.exe"
)

foreach ($bin in $GlibBins) {
    $src = Join-Path $BinDir $bin
    if (Test-Path $src) {
        Copy-Item $src -Destination "$OutputDir\bin\" -Force
        Write-Verbose "  Copied: $bin"
    }
}

# -----------------------------------------------------------------------------
# Copy GDK-Pixbuf loaders
# -----------------------------------------------------------------------------

Write-Log "Copying GDK-Pixbuf loaders..."

$PixbufDir = Join-Path $DepsRoot "lib\gdk-pixbuf-2.0\2.10.0\loaders"
if (Test-Path $PixbufDir) {
    Copy-Item "$PixbufDir\*.dll" -Destination "$OutputDir\lib\gdk-pixbuf-2.0\2.10.0\loaders\" -Force

    # Generate loaders.cache
    $QueryLoaders = Join-Path $BinDir "gdk-pixbuf-query-loaders.exe"
    if (Test-Path $QueryLoaders) {
        $loaders = Get-ChildItem "$OutputDir\lib\gdk-pixbuf-2.0\2.10.0\loaders\*.dll" |
                   ForEach-Object { $_.FullName }
        & $QueryLoaders $loaders > "$OutputDir\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache" 2>$null
    }
}

# -----------------------------------------------------------------------------
# Copy GTK4 modules
# -----------------------------------------------------------------------------

Write-Log "Copying GTK4 modules..."

$Gtk4MediaDir = Join-Path $DepsRoot "lib\gtk-4.0\4.0.0\media"
if (Test-Path $Gtk4MediaDir) {
    Copy-Item "$Gtk4MediaDir\*.dll" -Destination "$OutputDir\lib\gtk-4.0\4.0.0\media\" -Force -ErrorAction SilentlyContinue
}

$Gtk4PrintDir = Join-Path $DepsRoot "lib\gtk-4.0\4.0.0\printbackends"
if (Test-Path $Gtk4PrintDir) {
    Copy-Item "$Gtk4PrintDir\*.dll" -Destination "$OutputDir\lib\gtk-4.0\4.0.0\printbackends\" -Force -ErrorAction SilentlyContinue
}

# -----------------------------------------------------------------------------
# Copy GSettings schemas
# -----------------------------------------------------------------------------

Write-Log "Copying GSettings schemas..."

$SchemasDir = Join-Path $DepsRoot "share\glib-2.0\schemas"
if (Test-Path $SchemasDir) {
    Get-ChildItem "$SchemasDir\org.gtk.*" -ErrorAction SilentlyContinue |
        Copy-Item -Destination "$OutputDir\share\glib-2.0\schemas\" -Force
    Get-ChildItem "$SchemasDir\org.gnome.desktop.interface*" -ErrorAction SilentlyContinue |
        Copy-Item -Destination "$OutputDir\share\glib-2.0\schemas\" -Force
    Get-ChildItem "$SchemasDir\*.override" -ErrorAction SilentlyContinue |
        Copy-Item -Destination "$OutputDir\share\glib-2.0\schemas\" -Force
}

# -----------------------------------------------------------------------------
# Copy Icon themes
# -----------------------------------------------------------------------------

Write-Log "Copying icon themes..."

$AdwaitaDir = Join-Path $DepsRoot "share\icons\Adwaita"
if (Test-Path $AdwaitaDir) {
    Write-Log "  Copying Adwaita icons..."
    Copy-Item -Path $AdwaitaDir -Destination "$OutputDir\share\icons\" -Recurse -Force
}

$HicolorDir = Join-Path $DepsRoot "share\icons\hicolor"
if (Test-Path $HicolorDir) {
    Write-Log "  Copying hicolor icons..."
    Copy-Item -Path $HicolorDir -Destination "$OutputDir\share\icons\" -Recurse -Force
}

# -----------------------------------------------------------------------------
# Copy GTK4 and libadwaita settings
# -----------------------------------------------------------------------------

Write-Log "Copying GTK4 and libadwaita resources..."

$Gtk4Share = Join-Path $DepsRoot "share\gtk-4.0"
if (Test-Path $Gtk4Share) {
    Copy-Item -Path "$Gtk4Share\*" -Destination "$OutputDir\share\gtk-4.0\" -Recurse -Force -ErrorAction SilentlyContinue
}

$AdwaitaShare = Join-Path $DepsRoot "share\libadwaita-1"
if (Test-Path $AdwaitaShare) {
    Copy-Item -Path "$AdwaitaShare\*" -Destination "$OutputDir\share\libadwaita-1\" -Recurse -Force -ErrorAction SilentlyContinue
}

# -----------------------------------------------------------------------------
# Copy locale data
# -----------------------------------------------------------------------------

Write-Log "Copying locale data..."

$Locales = @("en", "en_US", "en_GB", "de", "es", "fr", "it", "ja", "ko", "pt_BR", "ru", "zh_CN", "zh_TW")
$LocaleDir = Join-Path $DepsRoot "share\locale"
$Domains = @("gtk40", "gtk40-properties", "glib20", "libadwaita")

foreach ($locale in $Locales) {
    $src = Join-Path $LocaleDir $locale
    if (Test-Path $src) {
        foreach ($domain in $Domains) {
            $moFile = Join-Path $src "LC_MESSAGES\$domain.mo"
            if (Test-Path $moFile) {
                $destDir = "$OutputDir\share\locale\$locale\LC_MESSAGES"
                New-Item -ItemType Directory -Force -Path $destDir | Out-Null
                Copy-Item $moFile -Destination $destDir -Force
            }
        }
    }
}

# -----------------------------------------------------------------------------
# Calculate bundle size
# -----------------------------------------------------------------------------

Write-Log "Calculating bundle size..."

$BundleSize = (Get-ChildItem $OutputDir -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB
$DllCount = (Get-ChildItem "$OutputDir\bin\*.dll").Count

Write-Log "Bundle complete!"
Write-Log "  Location: $OutputDir"
Write-Log "  Total size: $([math]::Round($BundleSize, 2)) MB"
Write-Log "  DLLs bundled: $DllCount"

# List key DLLs
Write-Log "Key dependencies:"
@("libgtk-4", "libadwaita-1", "libglib-2.0", "libgio-2.0", "libgobject-2.0") | ForEach-Object {
    $pattern = "$OutputDir\bin\$_*.dll"
    if (Test-Path $pattern) {
        Write-Log "  - $_"
    }
}
