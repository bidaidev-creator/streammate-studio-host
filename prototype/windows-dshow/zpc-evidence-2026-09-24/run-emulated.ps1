$root='C:\Users\lloyd\streammate-pivot-37'
Start-Transcript "$root\emulated-transcript.txt"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root\automated-probe.ps1" -HostExe "$root\bundle\studio-host.exe"
$code=$LASTEXITCODE
@{probeExitCode=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$root\emulated-result.json"
Stop-Transcript
exit $code
