$share = '\\192.168.64.3\nostr-home'
# Force close cached connection
Get-SmbConnection -ServerName '192.168.64.3' -ErrorAction SilentlyContinue | ForEach-Object {
  Remove-SmbConnection -InputObject $_ -Force -Confirm:$false -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2

# Signature block again
$sig = @'
using System;
using System.Runtime.InteropServices;
public class WNetHelper2 {
    [StructLayout(LayoutKind.Sequential)]
    public struct NR { public int a; public int b; public int c; public int d;
        [MarshalAs(UnmanagedType.LPWStr)] public string ln;
        [MarshalAs(UnmanagedType.LPWStr)] public string rn;
        [MarshalAs(UnmanagedType.LPWStr)] public string cm;
        [MarshalAs(UnmanagedType.LPWStr)] public string pr; }
    [DllImport("mpr.dll", CharSet = CharSet.Unicode)]
    public static extern int WNetAddConnection2(ref NR r, string p, string u, int f);
    [DllImport("mpr.dll", CharSet = CharSet.Unicode)]
    public static extern int WNetCancelConnection2(string n, int f, bool force);
}
'@
if (-not ([System.Management.Automation.PSTypeName]'WNetHelper2').Type) { Add-Type -TypeDefinition $sig }
[WNetHelper2]::WNetCancelConnection2($share, 0, $true) | Out-Null
[WNetHelper2]::WNetCancelConnection2('Y:', 0, $true) | Out-Null

$nr = New-Object WNetHelper2+NR
$nr.b = 1  # RESOURCETYPE_DISK
$nr.ln = 'Y:'
$nr.rn = $share
$rc = [WNetHelper2]::WNetAddConnection2([ref]$nr, 'test-password-for-acceptance', 'nostr', 0)
Write-Host "W8-reconnect: WNetAddConnection2 rc = $rc  (0 == SUCCESS after server-restart)"
if ($rc -eq 0) {
  Get-ChildItem Y:\ -ErrorAction SilentlyContinue | Format-Table Mode,Length,Name -AutoSize
  Get-SmbConnection -ServerName '192.168.64.3' | Format-Table ServerName,ShareName,Dialect,NumOpens -AutoSize
}
