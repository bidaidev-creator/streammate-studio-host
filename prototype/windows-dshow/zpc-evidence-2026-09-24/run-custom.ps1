$root='C:\Users\lloyd\streammate-pivot-37'
Start-Transcript "$root\custom-transcript.txt"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root\automated-probe.ps1" -HostExe "$root\bundle\studio-host.exe" -Script 'device 0;custom;health;quit'
$code=$LASTEXITCODE
@{probeExitCode=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$root\custom-result.json"
$f=Get-ChildItem "$root\dshow-probe-*.log" | Sort LastWriteTime | Select -Last 1
Copy-Item $f.FullName "$root\custom-form.log"
Stop-Transcript
exit $code
