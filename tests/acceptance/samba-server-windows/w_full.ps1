$ErrorActionPreference = 'Continue'
$signature = @'
using System;
using System.Runtime.InteropServices;
public class WNetHelper {
    [StructLayout(LayoutKind.Sequential)]
    public struct NETRESOURCE {
        public int dwScope;
        public int dwType;
        public int dwDisplayType;
        public int dwUsage;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpLocalName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpRemoteName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpComment;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpProvider;
    }
    [DllImport("mpr.dll", CharSet = CharSet.Unicode)]
    public static extern int WNetAddConnection2(ref NETRESOURCE lpNetResource, string lpPassword, string lpUsername, int dwFlags);
    [DllImport("mpr.dll", CharSet = CharSet.Unicode)]
    public static extern int WNetCancelConnection2(string lpName, int dwFlags, bool fForce);
}
'@
if (-not ([System.Management.Automation.PSTypeName]'WNetHelper').Type) {
    Add-Type -TypeDefinition $signature
}

[WNetHelper]::WNetCancelConnection2('Y:', 0, $true) | Out-Null
[WNetHelper]::WNetCancelConnection2('\\192.168.64.3\nostr-home', 0, $true) | Out-Null

$nr = New-Object WNetHelper+NETRESOURCE
$nr.dwType = 1
$nr.lpLocalName = 'Y:'
$nr.lpRemoteName = '\\192.168.64.3\nostr-home'
$rc = [WNetHelper]::WNetAddConnection2([ref]$nr, 'test-password-for-acceptance', 'nostr', 0)
Write-Host "=== W1 mount ==="
Write-Host ("WNetAddConnection2 rc = {0}" -f $rc)

Write-Host "`n=== W7 signing/dialect ==="
Get-SmbConnection -ServerName '192.168.64.3' | Format-Table ServerName,ShareName,Dialect,Signing,Encrypted,NumOpens -AutoSize

Write-Host "`n=== W2 directory listing (via UNC) ==="
Get-ChildItem '\\192.168.64.3\nostr-home\' -Force | Format-Table Mode,Length,LastWriteTime,Name

Write-Host "`n=== W2 subdir listing ==="
Get-ChildItem '\\192.168.64.3\nostr-home\subdir' | Format-Table Mode,Length,LastWriteTime,Name

Write-Host "`n=== README content ==="
Get-Content '\\192.168.64.3\nostr-home\README.txt'

Write-Host "`n=== W3 write small file ==="
$smallPath = '\\192.168.64.3\nostr-home\w3_small.txt'
"Windows smoke write: $(Get-Date -Format 'o')" | Out-File $smallPath -Encoding utf8 -NoNewline
$smallHashWin = (Get-FileHash $smallPath -Algorithm SHA256).Hash
Write-Host "small file sha256 (Windows side): $smallHashWin"

Write-Host "`n=== W4 write 10 MB file, measure ==="
$largePath = '\\192.168.64.3\nostr-home\w4_large.bin'
$bytes = New-Object byte[] (10 * 1024 * 1024)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($bytes)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
[System.IO.File]::WriteAllBytes($largePath, $bytes)
$sw.Stop()
$sizeMB = $bytes.Length / 1MB
$secs = $sw.Elapsed.TotalSeconds
$mbps = $sizeMB / $secs
Write-Host ("10 MB write: {0:N2}s → {1:N2} MB/s" -f $secs, $mbps)
$largeHashWin = (Get-FileHash $largePath -Algorithm SHA256).Hash
Write-Host "large file sha256 (Windows side): $largeHashWin"

Write-Host "`n=== W5 rename + delete ==="
$renTo = '\\192.168.64.3\nostr-home\w5_small_renamed.txt'
Rename-Item -Path $smallPath -NewName 'w5_small_renamed.txt' -Force
Write-Host "renamed: $(Test-Path $renTo)"
Remove-Item -Path $renTo -Force
Write-Host "deleted: $((-not (Test-Path $renTo)))"

# Persist hashes to a temp file for later comparison
$hashes = @{ small = $smallHashWin; large = $largeHashWin }
$hashes | ConvertTo-Json | Out-File 'C:/Users/bizarro/w_hashes.json' -Encoding utf8

Write-Host "`n=== Get-SmbConnection final ==="
Get-SmbConnection | Format-Table ServerName,ShareName,Dialect,Signing,Encrypted,NumOpens -AutoSize
