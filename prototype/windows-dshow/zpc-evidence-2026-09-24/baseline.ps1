$ErrorActionPreference='Stop'
$root='C:\Users\lloyd\streammate-pivot-37'
Start-Transcript -Path "$root\baseline-transcript.txt"
@{utc=[DateTime]::UtcNow.ToString('o');user=[Security.Principal.WindowsIdentity]::GetCurrent().Name;sessionId=(Get-Process -Id $PID).SessionId;os=(Get-CimInstance Win32_OperatingSystem | Select Caption,Version);cameras=@(Get-PnpDevice -PresentOnly | Where-Object {$_.Class -in @('Camera','Image')} | Select Status,Class,FriendlyName)} | ConvertTo-Json -Depth 5 | Set-Content "$root\machine.json"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root\bundle\dshow-probe.ps1" -Script 'props;press video_config;health;press xbar_config;health;quit'
$code=$LASTEXITCODE
@{probeExitCode=$code;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$root\baseline-result.json"
Stop-Transcript
exit $code
