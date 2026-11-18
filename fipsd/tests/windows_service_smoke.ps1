<#
fipsd as a Windows service, end to end (nostrc-iq1n9.6.3; the nostrc-iq1n9.6
acceptance). Builds the ZIP from the build tree (packaging/windows/
build-zip.ps1), extracts it and installs with its install-service.ps1:
binaries into %ProgramFiles%\fips, the example config into a restricted
%ProgramData%\fips, the 'fipsd' service. Starts the service and reads, over
the control socket (fipsctl show status, TCP 127.0.0.1:21210), that it is
running with its Wintun adapter up. A second fipsd, a console process of the
installed binary with its own adapter and only the system directories on
PATH, peers with the service node over UDP on 127.0.0.1 and pings the
service node's fd00::/8 address from its own (ping -6 -S): each echo goes
into the second node's adapter, across the mesh, out of the service's
adapter and back (Windows sends by the strong host model, so the source pins
the path; windows_two_node_smoke.ps1 explains). Then the second node stops on
Ctrl+Break, the service through the SCM (STOPPED, exit code 0, a clean
shutdown in its log), and uninstall-service.ps1 -RemoveAll removes it.
Nothing may be left: no service, adapter, fd00::/8 route, firewall rule,
install or config directory, FIPS_CONFIG or PATH entry.

Skips (77) unless elevated and FIPSD_SERVICE_PRIVILEGED_SMOKE=1: it installs a
real service machine-wide. It refuses (1) to run where a fipsd service,
%ProgramFiles%\fips or %ProgramData%\fips already exists, or a Wintun adapter
it would name. -DllPath names the directory of the DLLs the build links
(MSYS2: <prefix>\bin); -WintunZip (or FIPSD_WINTUN_ZIP) a downloaded
wintun-0.14.1.zip, which build-zip.ps1 still checks. Exit 0 pass, 1 fail.
#>
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$DllPath = '',
    [string]$WintunZip = $env:FIPSD_WINTUN_ZIP,
    [string]$WorkDir = (Join-Path $env:TEMP 'fipsd-service-smoke')
)
$ErrorActionPreference = 'Stop'
function Skip([string]$Why) { Write-Output "SKIP: $Why"; exit 77 }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Skip 'installing a service needs an elevated (Administrator) session' }
if ($env:FIPSD_SERVICE_PRIVILEGED_SMOKE -ne '1') { Skip 'set FIPSD_SERVICE_PRIVILEGED_SMOKE=1 to install, run and remove the fipsd service' }

$ServiceName = 'fipsd'
$InstallDir = "$env:ProgramFiles\fips"
$ConfigDir = "$env:ProgramData\fips"
$Packaging = Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging\windows'
$Peer = @{ Tun = 'fipsSmokePeer'; Port = 21003; Nsec = 'nsec1qgry0amhkaqmh7q83wduc205c294yv2tg7ykqxtlfn8lc02gfeksn0fdqe'
           Addr = 'fd77:c520:19ea:4511:241:7a9e:7d18:2b13' }
$RuleName = 'fipsd-service-smoke-icmpv6'
$ServiceTun = 'fips0'

foreach ($blocker in @(
        @{ Present = [bool](Get-Service -Name $ServiceName -ErrorAction SilentlyContinue); What = "a $ServiceName service is installed" },
        @{ Present = (Test-Path -LiteralPath $InstallDir); What = "$InstallDir exists" },
        @{ Present = (Test-Path -LiteralPath $ConfigDir); What = "$ConfigDir exists" },
        @{ Present = [bool](Get-NetAdapter -Name $ServiceTun, $Peer.Tun -IncludeHidden -ErrorAction SilentlyContinue); What = "an adapter named $ServiceTun or $($Peer.Tun) exists" })) {
    if ($blocker.Present) { Write-Output "FAIL: $($blocker.What); this smoke installs and removes its own and will not touch an existing one"; exit 1 }
}

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "  ok: $What" } else { Write-Host "  FAIL: $What"; $failures.Add($What) }
}
# A harness wait on state only the OS, the daemon's log or its control socket
# can show: bounded, re-checked every 200 ms.
function Wait-For([scriptblock]$Condition, [int]$Seconds, [string]$What) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while (-not (& $Condition)) {
        if ((Get-Date) -gt $deadline) { Check $false "$What (within $Seconds s)"; return $false }
        Start-Sleep -Milliseconds 200
    }
    return $true
}
function Test-LogMatch([string]$Path, [string]$Pattern) { (Test-Path -LiteralPath $Path) -and ((Get-Content -LiteralPath $Path -Raw) -match $Pattern) }
function Fipsctl([string[]]$Arguments) {
    $out = & "$InstallDir\fipsctl.exe" @Arguments 2>&1
    return @{ Code = $LASTEXITCODE; Text = ($out | ForEach-Object { "$_" }) -join "`n" }
}
function Status {
    $r = Fipsctl @('show', 'status')
    if ($r.Code -ne 0) { return $null }
    try { return $r.Text | ConvertFrom-Json } catch { return $null }
}
function Received([string]$Tun) { [int64](Get-NetAdapterStatistics -Name $Tun).ReceivedUnicastPackets }
# Ctrl+Break (the daemon's SIGINT) to a daemon on its own console, from a
# helper that detaches from ours and attaches to its console
# (windows_two_node_smoke.ps1).
function Send-CtrlBreak([int]$TargetPid) {
    $code = 'Add-Type -Namespace W3 -Name K -MemberDefinition ''[DllImport("kernel32.dll")] public static extern bool FreeConsole(); ' +
        '[DllImport("kernel32.dll")] public static extern bool AttachConsole(uint p); ' +
        '[DllImport("kernel32.dll")] public static extern bool SetConsoleCtrlHandler(System.IntPtr h, bool add); ' +
        '[DllImport("kernel32.dll")] public static extern bool GenerateConsoleCtrlEvent(uint e, uint g);''; ' +
        '[void][W3.K]::FreeConsole(); if (-not [W3.K]::AttachConsole(' + $TargetPid + ')) { exit 2 }; ' +
        '[void][W3.K]::SetConsoleCtrlHandler([System.IntPtr]::Zero, $true); if (-not [W3.K]::GenerateConsoleCtrlEvent(1, 0)) { exit 3 }; exit 0'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    return (Start-Process powershell.exe -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded `
        -WindowStyle Hidden -Wait -PassThru).ExitCode
}

if (Test-Path -LiteralPath $WorkDir) { Remove-Item -Recurse -Force -LiteralPath $WorkDir }
New-Item -ItemType Directory -Path $WorkDir | Out-Null
$ServiceLog = Join-Path $WorkDir 'service-fips.log'
$PeerLog = Join-Path $WorkDir 'peer.log'
$peerProcess = $null
$installed = $false
try {
    Write-Output '== 1. package (build-zip.ps1)'
    $zipArgs = @{ BuildDir = $BuildDir; OutDir = $WorkDir }
    if ($DllPath) { $zipArgs.DllPath = @($DllPath) }
    if ($WintunZip) { $zipArgs.WintunZip = $WintunZip }
    & (Join-Path $Packaging 'build-zip.ps1') @zipArgs
    $zip = Get-ChildItem -LiteralPath $WorkDir -Filter 'fipsd-*-windows-*.zip' | Select-Object -First 1
    Check ($null -ne $zip) 'build-zip.ps1 wrote fipsd-<version>-windows-<arch>.zip'
    $pkg = Join-Path $WorkDir 'pkg'
    Expand-Archive -LiteralPath $zip.FullName -DestinationPath $pkg
    foreach ($f in @('fipsd.exe', 'fipsctl.exe', 'wintun.dll', 'wintun-LICENSE.txt', 'fips.yaml', 'hosts',
                     'install-service.ps1', 'uninstall-service.ps1', 'README.txt')) {
        Check (Test-Path -LiteralPath (Join-Path $pkg $f) -PathType Leaf) "the ZIP carries $f"
    }
    Write-Output "  fipstop.exe $(if (Test-Path -LiteralPath (Join-Path $pkg 'fipstop.exe')) { 'included' } else { 'not built' }); DLLs: $((Get-ChildItem -LiteralPath $pkg -Filter '*.dll' | ForEach-Object Name) -join ', ')"
    Check (-not (Select-String -LiteralPath (Join-Path $pkg 'README.txt') -Pattern '^\s*sc (start|stop|query)\b' -Quiet)) 'README.txt names sc.exe, not a bare sc'

    Write-Output '== 2. install (install-service.ps1)'
    $installed = $true
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'install-service.ps1')
    Check ($LASTEXITCODE -eq 0) "install-service.ps1 exited $LASTEXITCODE"
    $qc = (& sc.exe qc $ServiceName) -join "`n"
    Check ($qc -match [regex]::Escape("`"$InstallDir\fipsd.exe`" --service")) 'the service runs the installed fipsd.exe --service'
    Check ($qc -match 'AUTO_START' -and $qc -match 'LocalSystem') 'automatic start as LocalSystem'
    $acl = Get-Acl -LiteralPath $ConfigDir
    $sids = @($acl.Access | ForEach-Object { $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value } | Sort-Object -Unique)
    Check ($acl.AreAccessRulesProtected -and (($sids -join ',') -eq 'S-1-5-18,S-1-5-32-544')) "$ConfigDir is SYSTEM and Administrators only ($($sids -join ', '))"
    Check ([Environment]::GetEnvironmentVariable('FIPS_CONFIG', 'Machine') -eq "$ConfigDir\fips.yaml") 'FIPS_CONFIG names the installed config'
    Check (Test-Path -LiteralPath "$ConfigDir\fips.yaml") 'the example config was installed'

    Write-Output '== 3. start the service; fipsctl show status over TCP 127.0.0.1:21210'
    Start-Service -Name $ServiceName
    (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(60))
    Check ((Get-Service -Name $ServiceName).Status -eq 'Running') 'the SCM reports the service running'
    $null = Wait-For { $s = Status; $s -and $s.tun_state -eq 'active' } 60 'fipsctl show status reports tun_state active'
    $status = Status
    Write-Output ("  state={0} tun_state={1} tun_name={2} effective_ipv6_mtu={3} ipv6_addr={4} npub={5}" -f `
        $status.state, $status.tun_state, $status.tun_name, $status.effective_ipv6_mtu, $status.ipv6_addr, $status.npub)
    Check ($status.state -eq 'running') 'the node is running'
    Check ($status.tun_name -eq $ServiceTun) "the TUN is $ServiceTun"
    Check ([string]$status.ipv6_addr -like 'fd*') 'the node has an fd00::/8 address'
    $ip = Get-NetIPAddress -InterfaceAlias $ServiceTun -AddressFamily IPv6 -IPAddress $status.ipv6_addr -ErrorAction SilentlyContinue
    Check ($ip -and $ip.PrefixLength -eq 128) "$ServiceTun carries $($status.ipv6_addr)/128"
    $listen = Get-NetTCPConnection -State Listen -LocalPort 21210 -ErrorAction SilentlyContinue
    Check (@($listen).Count -eq 1 -and $listen.LocalAddress -eq '127.0.0.1') 'the control socket listens on 127.0.0.1:21210 only'
    $r = Fipsctl @('show', 'transports')
    Check ($r.Code -eq 0 -and $r.Text -match '"type": "udp"') 'fipsctl show transports'

    Write-Output "== 4. a second node ($($Peer.Tun), console) peers over UDP 127.0.0.1"
    $peerConfig = Join-Path $WorkDir 'peer.yaml'
    @"
node:
  identity: {nsec: '$($Peer.Nsec)', persistent: false}
  control: {enabled: false}
tun: {enabled: true, name: $($Peer.Tun)}
dns: {enabled: false}
transports:
  udp: {bind_addr: '127.0.0.1:$($Peer.Port)'}
peers:
  - npub: '$($status.npub)'
    addresses: [{transport: udp, addr: '127.0.0.1:2121'}]
"@ | Set-Content -Encoding ascii -LiteralPath $peerConfig
    $savedPath = $env:PATH
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot" # the package must carry its DLLs
    try {
        $peerProcess = Start-Process -FilePath "$InstallDir\fipsd.exe" -ArgumentList '-c', "`"$peerConfig`"", '--log-file', "`"$PeerLog`"" `
            -WindowStyle Hidden -PassThru
    } finally { $env:PATH = $savedPath }
    Write-Output "  pid $($peerProcess.Id), $($Peer.Tun), $($Peer.Addr)"
    if (-not (Wait-For { $peerProcess.HasExited -or (Test-LogMatch $PeerLog 'IPv6 shim up on') } 60 'the second node is up') -or $peerProcess.HasExited) {
        throw "the second node is not up (exit code $(if ($peerProcess.HasExited) { $peerProcess.ExitCode } else { 'none' }))"
    }
    $null = Wait-For { Test-LogMatch $PeerLog 'link established with' } 60 'the second node links to the service node'
    $null = Wait-For { $s = Status; $s -and $s.peer_count -ge 1 } 60 'fipsctl show status counts the peer'

    Write-Output "== 5. ping -6 -S $($Peer.Addr) $($status.ipv6_addr)"
    New-NetFirewallRule -Name $RuleName -DisplayName 'fipsd service smoke: ICMPv6 echo in' -Direction Inbound `
        -Protocol ICMPv6 -IcmpType 128 -InterfaceAlias $ServiceTun, $Peer.Tun -Action Allow -Profile Any | Out-Null
    $route = Find-NetRoute -RemoteIPAddress $status.ipv6_addr -LocalIPAddress $Peer.Addr | Where-Object { $_.DestinationPrefix }
    Check ($route.InterfaceAlias -eq $Peer.Tun) "Windows sends $($Peer.Addr) -> $($status.ipv6_addr) through $($Peer.Tun)"
    $before = @{ Service = Received $ServiceTun; Peer = Received $Peer.Tun }
    $out = (& ping.exe -6 -n 3 -w 1500 -S $Peer.Addr $status.ipv6_addr) -join "`n"
    Write-Output $out
    $wrote = @{ Service = (Received $ServiceTun) - $before.Service; Peer = (Received $Peer.Tun) - $before.Peer }
    Write-Output "  shims wrote: service +$($wrote.Service) into $ServiceTun (requests), peer +$($wrote.Peer) into $($Peer.Tun) (replies)"
    Check ($out -match 'Received = 3') '3/3 replies'
    Check ($wrote.Service -ge 3 -and $wrote.Peer -ge 3) 'every request and reply crossed the mesh'
    $s = Status
    Check ($s.session_count -ge 1) "the service node holds a session ($($s.session_count))"
}
catch { Check $false "$_" }
finally {
    Remove-NetFirewallRule -Name $RuleName -ErrorAction SilentlyContinue
    Write-Output '== 6. stop: the second node on Ctrl+Break, the service through the SCM'
    if ($peerProcess -and -not $peerProcess.HasExited) {
        $rc = Send-CtrlBreak $peerProcess.Id
        if (-not $peerProcess.WaitForExit(30000)) { Check $false "the second node exits on Ctrl+Break (helper $rc)"; Stop-Process -Force -Id $peerProcess.Id }
        else { Check ($peerProcess.ExitCode -eq 0) "the second node exited $($peerProcess.ExitCode)" }
    }
    try {
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
            if ((Get-Service -Name $ServiceName).Status -ne 'Stopped') {
                Stop-Service -Name $ServiceName -Force
                (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60))
            }
            $cim = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
            Check ($cim.State -eq 'Stopped' -and $cim.ExitCode -eq 0) "the service stopped with exit code $($cim.ExitCode)"
        }
    } catch { Check $false "stopping the service: $_" }
    if (Test-Path -LiteralPath "$ConfigDir\fips.log") { Copy-Item -LiteralPath "$ConfigDir\fips.log" -Destination $ServiceLog }
    if ($installed) {
        Check (Test-LogMatch $ServiceLog 'running as the fipsd service') 'the service log shows it ran under the SCM'
        Check (Test-LogMatch $ServiceLog 'control socket listening at 127\.0\.0\.1:21210') 'the service log shows the control socket'
        Check (Test-LogMatch $ServiceLog 'service stop requested') 'the service log shows the SCM stop'
        Check (Test-LogMatch $ServiceLog 'fipsd shutdown complete \(exit 0\)') 'the service shut down cleanly'
        Check (-not (Select-String -LiteralPath $ServiceLog -CaseSensitive -Pattern ' ERROR ' -Quiet)) 'the service logged no error'
        Write-Host '-- service log (service, control, tun, link, shutdown lines)'
        Get-Content -LiteralPath $ServiceLog | Select-String -CaseSensitive -Pattern 'service|control socket| tun: |ipv6/|link established|runtime stopped|shutdown complete| ERROR | WARN ' |
            ForEach-Object { Write-Host "   $_" }
    }

    Write-Output '== 7. uninstall (uninstall-service.ps1 -RemoveAll)'
    if (Test-Path -LiteralPath "$InstallDir\uninstall-service.ps1") {
        try {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$InstallDir\uninstall-service.ps1" -RemoveAll
            Check ($LASTEXITCODE -eq 0) "uninstall-service.ps1 exited $LASTEXITCODE"
        } catch { Check $false "uninstall-service.ps1: $_" }
    }
    if ($installed) {
        # Leave the host as found even after a failure above.
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) { & sc.exe delete $ServiceName | Out-Null; Check $false 'the service was removed' }
        foreach ($dir in @($InstallDir, $ConfigDir)) {
            if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir; Check $false "$dir was removed" }
        }
        foreach ($tun in @($ServiceTun, $Peer.Tun)) {
            $null = Wait-For { -not (Get-NetAdapter -Name $tun -IncludeHidden -ErrorAction SilentlyContinue) } 10 "$tun removed"
            Check (-not (Get-NetRoute -InterfaceAlias $tun -ErrorAction SilentlyContinue)) "no route left on $tun"
        }
        Check (-not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) 'no fipsd service left'
        Check (-not (Get-NetFirewallRule -Name $RuleName -ErrorAction SilentlyContinue)) 'no firewall rule left'
        Check (-not [Environment]::GetEnvironmentVariable('FIPS_CONFIG', 'Machine')) 'FIPS_CONFIG removed'
        Check ((([Environment]::GetEnvironmentVariable('Path', 'Machine')) -split ';') -notcontains $InstallDir) 'PATH entry removed'
    }
    if (Test-Path -LiteralPath $PeerLog) {
        Check (-not (Select-String -LiteralPath $PeerLog -CaseSensitive -Pattern ' ERROR ' -Quiet)) 'the second node logged no error'
    }
}
if ($failures.Count) { Write-Output "service smoke: FAILED ($($failures.Count))"; exit 1 }
Write-Output 'service smoke: ok'
exit 0
