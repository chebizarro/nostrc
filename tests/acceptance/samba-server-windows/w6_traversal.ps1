# W6: path traversal + write escape attempts. Success = ALL of these fail/deny.
$share = '\\192.168.64.3\nostr-home'
Write-Host '=== W6a: list parent via .. ==='
try { Get-ChildItem "$share\.." -ErrorAction Stop 2>&1 | Out-String | Write-Host } catch { Write-Host "W6a DENIED: $($_.Exception.Message)" }
Write-Host ''
Write-Host '=== W6b: read passdb via .. ==='
try { Get-Content "$share\..\smbpasswd" -ErrorAction Stop -TotalCount 3 2>&1 } catch { Write-Host "W6b DENIED: $($_.Exception.Message)" }
Write-Host ''
Write-Host '=== W6c: write escape file via .. ==='
try { 'escape' | Out-File "$share\..\escape.txt" -ErrorAction Stop } catch { Write-Host "W6c DENIED: $($_.Exception.Message)" }
Write-Host ''
Write-Host '=== W6d: enumerate hidden dirs at share root ==='
Get-ChildItem $share -Force -Attributes Hidden -ErrorAction SilentlyContinue | Format-Table Mode,Length,Name
