$ErrorActionPreference = 'Continue'
$signature = @'
using System;
using System.Runtime.InteropServices;
public class WNetHelper {
    [StructLayout(LayoutKind.Sequential)]
    public struct NETRESOURCE {
        public int dwScope; public int dwType; public int dwDisplayType; public int dwUsage;
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
if (-not ([System.Management.Automation.PSTypeName]'WNetHelper').Type) { Add-Type -TypeDefinition $signature }

[WNetHelper]::WNetCancelConnection2('\\192.168.64.3\nostr-home', 0, $true) | Out-Null
$nr = New-Object WNetHelper+NETRESOURCE
$nr.dwType = 1
$nr.lpRemoteName = '\\192.168.64.3\nostr-home'
$rc = [WNetHelper]::WNetAddConnection2([ref]$nr, 'test-password-for-acceptance', 'nostr', 0)
Write-Host "initial connect rc=$rc"

Write-Host "=== W8/W9 pre-check ==="
$hashBefore = (Get-FileHash '\\192.168.64.3\nostr-home\w4_large.bin' -Algorithm SHA256).Hash
Write-Host "hash BEFORE: $hashBefore"

Write-Host "=== Waiting for server restart (5s) ==="
Start-Sleep -Seconds 5

Write-Host "=== Attempt access during outage window (server should be down or just recovered) ==="
try {
  $items = Get-ChildItem '\\192.168.64.3\nostr-home' -ErrorAction Stop
  Write-Host "reconnect succeeded, items=$($items.Count)"
} catch {
  Write-Host "access during outage: $($_.Exception.Message)"
}

Write-Host "=== Waiting further (15s more) for server to be fully back ==="
Start-Sleep -Seconds 15

Write-Host "=== Retry after outage window (W8/W9) ==="
try {
  $items = Get-ChildItem '\\192.168.64.3\nostr-home' -ErrorAction Stop
  Write-Host "post-restart listing:"
  $items | Format-Table Name,Length,LastWriteTime
} catch {
  Write-Host "post-restart access FAILED: $($_.Exception.Message)"
}

$hashAfter = (Get-FileHash '\\192.168.64.3\nostr-home\w4_large.bin' -Algorithm SHA256 -ErrorAction Continue).Hash
Write-Host "hash AFTER: $hashAfter"
if ($hashBefore -eq $hashAfter) {
    Write-Host "W9 RETENTION: PASS (hash unchanged across restart)"
} else {
    Write-Host "W9 RETENTION: FAIL"
}

Write-Host "=== Get-SmbConnection post-restart ==="
Get-SmbConnection | Format-Table ServerName,ShareName,Dialect,Signing,Encrypted,NumOpens -AutoSize
