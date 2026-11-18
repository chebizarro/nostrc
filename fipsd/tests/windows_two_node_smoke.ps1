<#
Two fipsd nodes on one Windows host (nostrc-iq1n9.6.2): two processes, UDP
over 127.0.0.1 between them, each with its own Wintun adapter (fipsA, fipsB).
Skips (77) unless elevated and FIPSD_TUN_PRIVILEGED_SMOKE=1; wintun.dll must
sit beside fipsd.exe (and -DllPath names the directory of the DLLs it links,
e.g. MSYS2's <prefix>\bin). Exit 0 pass, 1 fail.

Both nodes' /128s live in one IP stack, and both adapters route fd00::/8.
Windows sends by the strong host model: a packet whose source is A's address
leaves through fipsA, the adapter that owns it, even to an address another
local adapter owns. So `ping -S <A> <B>` goes into fipsA, node A's shim reads
it, the mesh carries it over UDP, node B's shim writes it into fipsB, Windows
answers from B's address out of fipsB, and the reply crosses the mesh back:
a round trip through both adapters and both daemons, counted by each adapter's
receive counter (packets a shim wrote into it). The same holds from B to A and
for a TCP connection between sockets bound to the two addresses. Windows also
keeps neighbor unreachability detection on for the peer address it talks to (it
cannot be turned off on a Wintun interface), so now and then a unicast Neighbor
Solicitation and its Advertisement cross the mesh between the two addresses
too; the idle step reports them and they only add to the counts. (Without -S Windows may pick
either adapter, or answer its own address locally: the source pins the path.)
Inbound ICMPv6 echo and the test's TCP port on the two adapters are allowed by
firewall rules for the run (removed at the end). Both daemons then get a
Ctrl+Break, shut down cleanly (exit 0, no error logged) and leave no adapter
or route behind.
#>
param(
    [Parameter(Mandatory = $true)][string]$Fipsd,
    [string]$WorkDir = (Join-Path $env:TEMP 'fipsd-two-node'),
    # Directory of the runtime DLLs fipsd.exe links (MSYS2: <prefix>\bin), put first on PATH.
    [string]$DllPath = ''
)
$ErrorActionPreference = 'Stop'
function Skip([string]$Why) { Write-Output "SKIP: $Why"; exit 77 }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Skip 'Wintun adapters need an elevated (Administrator) session' }
if ($env:FIPSD_TUN_PRIVILEGED_SMOKE -ne '1') { Skip 'set FIPSD_TUN_PRIVILEGED_SMOKE=1 to create real adapters' }
$Fipsd = (Resolve-Path $Fipsd).Path
if (-not (Test-Path (Join-Path (Split-Path $Fipsd) 'wintun.dll'))) { Write-Output "FAIL: no wintun.dll beside $Fipsd"; exit 1 }

# Throwaway test identities (fipsctl keygen --stdout; fipsctl address <npub>).
$nodes = @(
    @{ Name = 'A'; Tun = 'fipsA'; Port = 21001; Nsec = 'nsec1mq40vushygr5qkgxs9vhpyl8svywya5swyuq53eqm2wdl4devr5stagvyl'
       Npub = 'npub1l6nvaegmj0dj7gys4pcmlfancq9pgwcrauqefmkvpsdpufetu80qu6snx6'; Addr = 'fdb3:64c1:9876:83d8:b55c:e4bd:eb33:3e21' },
    @{ Name = 'B'; Tun = 'fipsB'; Port = 21002; Nsec = 'nsec1qgry0amhkaqmh7q83wduc205c294yv2tg7ykqxtlfn8lc02gfeksn0fdqe'
       Npub = 'npub18n705jhnkel2dcguqej4y0mdqt9ccfulc3s7q2a7zn9nytn9v88qxgtuy4'; Addr = 'fd77:c520:19ea:4511:241:7a9e:7d18:2b13' }
)
$A = $nodes[0]; $B = $nodes[1]
$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "  ok: $What" } else { Write-Host "  FAIL: $What"; $failures.Add($What) }
}
function Log-Has([hashtable]$Node, [string]$Pattern) { (Test-Path $Node.Log) -and ((Get-Content $Node.Log -Raw) -match $Pattern) }
# A harness wait on state only the OS or the daemon's log can show (no
# control socket on Windows yet): bounded, re-checked every 200 ms.
function Wait-For([scriptblock]$Condition, [int]$Seconds, [string]$What) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while (-not (& $Condition)) {
        if ((Get-Date) -gt $deadline) { Check $false "$What (within $Seconds s)"; return $false }
        Start-Sleep -Milliseconds 200
    }
    return $true
}
function Counters([string]$Tun) {
    $s = Get-NetAdapterStatistics -Name $Tun
    return @{ Sent = [int64]$s.SentUnicastPackets; Received = [int64]$s.ReceivedUnicastPackets }
}
function Ping6([string]$From, [string]$To) {
    $out = (& ping.exe -6 -n 3 -w 1500 -S $From $To) -join "`n"
    Write-Output $out
}
# Ctrl+Break (the daemon's SIGINT) to a daemon on its own console, from a
# helper that detaches from ours, attaches to its console and ignores the
# event itself. (CTRL_C_EVENT does not reach a process ShellExecute started.)
function Send-CtrlBreak([int]$TargetPid) {
    $code = 'Add-Type -Namespace W2 -Name K -MemberDefinition ''[DllImport("kernel32.dll")] public static extern bool FreeConsole(); ' +
        '[DllImport("kernel32.dll")] public static extern bool AttachConsole(uint p); ' +
        '[DllImport("kernel32.dll")] public static extern bool SetConsoleCtrlHandler(System.IntPtr h, bool add); ' +
        '[DllImport("kernel32.dll")] public static extern bool GenerateConsoleCtrlEvent(uint e, uint g);''; ' +
        '[void][W2.K]::FreeConsole(); if (-not [W2.K]::AttachConsole(' + $TargetPid + ')) { exit 2 }; ' +
        '[void][W2.K]::SetConsoleCtrlHandler([System.IntPtr]::Zero, $true); if (-not [W2.K]::GenerateConsoleCtrlEvent(1, 0)) { exit 3 }; exit 0'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    return (Start-Process powershell.exe -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded `
        -WindowStyle Hidden -Wait -PassThru).ExitCode
}
# Packets the shims wrote into the two adapters since $Before.
function Written([hashtable]$Before) {
    $now = @{ A = (Counters $A.Tun).Received; B = (Counters $B.Tun).Received }
    return @{ A = $now.A - $Before.A; B = $now.B - $Before.B }
}
function Mark { return @{ A = (Counters $A.Tun).Received; B = (Counters $B.Tun).Received } }
# One TCP exchange between sockets bound to the two mesh addresses.
# (PowerShell names are case-insensitive: no local may shadow a parameter.)
function Tcp-Echo([hashtable]$From, [hashtable]$To, [int]$Port) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Parse($To.Addr), $Port)
    $listener.Start()
    $tcp = [Net.Sockets.TcpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Parse($From.Addr), 0))
    try {
        $connect = $tcp.ConnectAsync([Net.IPAddress]::Parse($To.Addr), $Port)
        $accept = $listener.AcceptTcpClientAsync()
        if (-not $accept.Wait(15000) -or -not $connect.Wait(15000)) { return 'no connection within 15 s' }
        $conn = $accept.Result
        $seen = $conn.Client.RemoteEndPoint.Address.ToString()
        $message = [Text.Encoding]::ASCII.GetBytes('over the fips mesh')
        $tcp.GetStream().Write($message, 0, $message.Length)
        $buffer = New-Object byte[] 64
        $conn.GetStream().ReadTimeout = 10000; $n = $conn.GetStream().Read($buffer, 0, $buffer.Length)
        $conn.GetStream().Write($buffer, 0, $n)
        $back = New-Object byte[] 64
        $tcp.GetStream().ReadTimeout = 10000; $m = $tcp.GetStream().Read($back, 0, $back.Length)
        $conn.Close()
        return "server saw $seen; echoed '$([Text.Encoding]::ASCII.GetString($back, 0, $m))'"
    } finally { $tcp.Close(); $listener.Stop() }
}
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory $WorkDir | Out-Null
foreach ($n in $nodes) {
    if (Get-NetAdapter -Name $n.Tun -IncludeHidden -ErrorAction SilentlyContinue) { Write-Output "FAIL: an adapter named $($n.Tun) already exists"; exit 1 }
    $peer = if ($n -eq $A) { $B } else { $A }
    $n.Config = Join-Path $WorkDir "$($n.Name).yaml"; $n.Log = Join-Path $WorkDir "$($n.Name).log"
    @"
node:
  identity: {nsec: '$($n.Nsec)', persistent: false}
  control: {enabled: false}
tun: {enabled: true, name: $($n.Tun)}
dns: {enabled: false}
transports:
  udp: {bind_addr: '127.0.0.1:$($n.Port)'}
peers:
  - npub: '$($peer.Npub)'
    addresses: [{transport: udp, addr: '127.0.0.1:$($peer.Port)'}]
"@ | Set-Content -Encoding ascii $n.Config
}
$env:FIPSD_LOG_LEVEL = 'debug'
if ($DllPath) { $env:PATH = "$DllPath;$env:PATH" }
try {
    foreach ($n in $nodes) {
        $n.Process = Start-Process -FilePath $Fipsd -ArgumentList '-c', "`"$($n.Config)`"", '--log-file', "`"$($n.Log)`"" `
            -WindowStyle Hidden -PassThru
        Write-Output "node $($n.Name): pid $($n.Process.Id), $($n.Tun), $($n.Addr)"
    }
    Write-Output '== bring-up'
    foreach ($n in $nodes) {
        if (-not (Wait-For { $n.Process.HasExited -or (Log-Has $n 'IPv6 shim up on') } 60 "node $($n.Name) shim up on $($n.Tun)") -or
            $n.Process.HasExited) { throw "node $($n.Name) is not up (exit code $(if ($n.Process.HasExited) { $n.Process.ExitCode } else { 'none' }))" }
        if (-not (Wait-For { Log-Has $n 'link established with' } 60 "node $($n.Name) link established")) { throw "node $($n.Name) has no link" }
        $ip = Get-NetIPAddress -InterfaceAlias $n.Tun -AddressFamily IPv6 -IPAddress $n.Addr -ErrorAction SilentlyContinue
        Check ($ip -and $ip.PrefixLength -eq 128 -and $ip.AddressState -eq 'Preferred') "$($n.Tun) carries $($n.Addr)/128, preferred"
        $route = Get-NetRoute -InterfaceAlias $n.Tun -DestinationPrefix 'fd00::/8' -ErrorAction SilentlyContinue
        Check ($null -ne $route) "$($n.Tun) has the fd00::/8 route"
    }
    Get-NetRoute -DestinationPrefix 'fd00::/8' | Format-Table -AutoSize ifIndex, InterfaceAlias, DestinationPrefix, NextHop, RouteMetric | Out-String | Write-Output

    $port = 21080
    New-NetFirewallRule -Name 'fipsd-two-node-smoke-icmpv6' -DisplayName 'fipsd two-node smoke: ICMPv6 echo in' -Direction Inbound `
        -Protocol ICMPv6 -IcmpType 128 -InterfaceAlias $A.Tun, $B.Tun -Action Allow -Profile Any | Out-Null
    New-NetFirewallRule -Name 'fipsd-two-node-smoke-tcp' -DisplayName 'fipsd two-node smoke: TCP in' -Direction Inbound `
        -Protocol TCP -LocalPort $port -InterfaceAlias $A.Tun, $B.Tun -Action Allow -Profile Any | Out-Null
    $pings = 3
    $step = 0
    foreach ($dir in @(@($A, $B), @($B, $A))) {
        $src = $dir[0]; $dst = $dir[1]; ++$step
        Write-Output "== $step. ping -S $($src.Name) $($dst.Name): out of $($src.Tun), across the mesh, back"
        $route = Find-NetRoute -RemoteIPAddress $dst.Addr -LocalIPAddress $src.Addr | Where-Object { $_.DestinationPrefix }
        Check ($route.InterfaceAlias -eq $src.Tun) "Windows sends $($src.Addr) -> $($dst.Addr) through $($src.Tun)"
        $before = Mark
        $out = Ping6 $src.Addr $dst.Addr; Write-Output $out
        $w = Written $before
        Write-Output "  shims wrote: node $($dst.Name) +$($w[$dst.Name]) into $($dst.Tun) (requests), node $($src.Name) +$($w[$src.Name]) into $($src.Tun) (replies)"
        Check ($out -match "Received = $pings") "$pings/$pings replies"
        Check ($w[$dst.Name] -ge $pings -and $w[$src.Name] -ge $pings) 'every request and reply crossed the mesh'
    }
    Write-Output '== 3. idle: neighbor unreachability probes between the two addresses'
    $before = Mark
    Start-Sleep -Seconds 3 # the length of one three-ping step
    $w = Written $before
    Write-Output "  shims wrote +$($w.A) into fipsA, +$($w.B) into fipsB while idle (reported, not judged)"
    Write-Output "== 4. TCP from $($A.Addr) to [$($B.Addr)]:$port"
    $before = Mark
    $result = Tcp-Echo $A $B $port
    $w = Written $before
    Write-Output "  $result; shims wrote +$($w.B) into fipsB, +$($w.A) into fipsA"
    Check ($result -match "server saw $([regex]::Escape($A.Addr)); echoed 'over the fips mesh'") 'TCP connect, send and echo across the mesh'
    Check ($w.A -ge 2 -and $w.B -ge 2) 'the TCP segments crossed the mesh both ways'
}
catch { Check $false "$_" }
finally {
    Remove-NetFirewallRule -Name 'fipsd-two-node-smoke-icmpv6', 'fipsd-two-node-smoke-tcp' -ErrorAction SilentlyContinue
    Write-Output '== shutdown (Ctrl+Break)'
    foreach ($n in $nodes) {
        if ($n.Process -and -not $n.Process.HasExited) {
            $rc = Send-CtrlBreak $n.Process.Id
            if (-not $n.Process.WaitForExit(30000)) { Check $false "node $($n.Name) exits on Ctrl+Break (helper $rc)"; Stop-Process -Force -Id $n.Process.Id }
            else { Check ($n.Process.ExitCode -eq 0) "node $($n.Name) exited $($n.Process.ExitCode) after Ctrl+Break" }
        }
    }
    foreach ($n in $nodes) {
        $null = Wait-For { -not (Get-NetAdapter -Name $n.Tun -IncludeHidden -ErrorAction SilentlyContinue) } 10 "$($n.Tun) removed"
        Check (-not (Get-NetAdapter -Name $n.Tun -IncludeHidden -ErrorAction SilentlyContinue)) "$($n.Tun) removed"
        Check (-not (Get-NetRoute -InterfaceAlias $n.Tun -ErrorAction SilentlyContinue)) "no route left on $($n.Tun)"
        if (Test-Path $n.Log) {
            Check (-not (Select-String -Path $n.Log -CaseSensitive -Pattern ' ERROR |teardown failed' -Quiet)) "node $($n.Name) logged no error"
            Write-Host "-- node $($n.Name) log (tun, ipv6, link, shutdown lines)"
            Get-Content $n.Log | Select-String -CaseSensitive -Pattern ' tun: [^w]|ipv6/|link established|SIGINT|runtime stopped| ERROR ' |
                ForEach-Object { Write-Host "   $_" }
        }
    }
}
if ($failures.Count) { Write-Output "two-node smoke: FAILED ($($failures.Count))"; exit 1 }
Write-Output 'two-node smoke: ok'
exit 0
