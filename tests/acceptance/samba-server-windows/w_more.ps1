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
Write-Host "connect rc=$rc"

Write-Host "`n=== W6 ACL escalation attempt (icacls granting Everyone Full) ==="
$file = '\\192.168.64.3\nostr-home\README.txt'
$out = icacls $file /grant '*S-1-1-0:F' 2>&1 | Out-String
Write-Host $out
Write-Host "expected: fail (server enforces valid users = @nostr-smb-share and does not honor NT ACL grants that raise privileges beyond the share's owner-fixed model)"

Write-Host "`n=== W6b Escalation: try to write outside share (should be impossible via UNC) ==="
# Try to write to \\192.168.64.3\somebogus or use path traversal
try {
  "traversal" | Out-File '\\192.168.64.3\nostr-home\..\evil.txt' -ErrorAction Stop
  Write-Host "traversal succeeded (BAD)"
} catch {
  Write-Host "traversal blocked: $($_.Exception.Message)"
}

Write-Host "`n=== W6c Escalation: attempt to access another user's file (no other users; verify same session limit) ==="
# Try to enumerate IPC$ hidden shares
try {
  Get-ChildItem '\\192.168.64.3\C$' -ErrorAction Stop
} catch {
  Write-Host "admin share access denied: $($_.Exception.Message)"
}

Write-Host "`n=== W9 retention baseline: hash before restart ==="
$largeHashBefore = (Get-FileHash '\\192.168.64.3\nostr-home\w4_large.bin' -Algorithm SHA256).Hash
Write-Host "hash before restart: $largeHashBefore"
$largeHashBefore | Out-File 'C:/Users/bizarro/w4_hash_before.txt' -Encoding ascii -NoNewline

Write-Host "`n=== Get-SmbConnection ==="
Get-SmbConnection | Format-Table ServerName,ShareName,Dialect,Signing,Encrypted,NumOpens -AutoSize
