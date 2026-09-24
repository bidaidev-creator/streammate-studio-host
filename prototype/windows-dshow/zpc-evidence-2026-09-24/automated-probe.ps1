# PROTOTYPE (streammate-pivot#37) - throwaway, never merge
# Windows PowerShell 5.1. One host and one control connection per session.
param(
    [string]$HostExe = (Join-Path $PSScriptRoot 'studio-host.exe'),
    [string]$Script = ''
)
$ErrorActionPreference = 'Stop'
$proofRoot = $PSScriptRoot
. (Join-Path $proofRoot 'ui-helper.ps1')
$script:commandIndex=0

$logPath = Join-Path $PSScriptRoot ('dshow-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.log')
# Process callbacks run on CLR threads, where PowerShell scriptblocks have no
# runspace. Keep the two output drains and serialized log writes in C#.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Concurrent;
using System.Text;
public class DShowProbeProcess : IDisposable {
    public Process Process;
    public ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
    private readonly object gate = new object();
    private readonly string log;
    public DShowProbeProcess(string logPath) { log = logPath; }
    public void Log(string text) {
        lock (gate) { File.AppendAllText(log, DateTime.UtcNow.ToString("o") + " " + text + Environment.NewLine, new UTF8Encoding(false)); }
    }
    public void Start(string exe, string token, string home) {
        var info = new ProcessStartInfo(exe, "--token " + token + " --port 0");
        info.WorkingDirectory = Path.GetDirectoryName(exe);
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        info.StandardOutputEncoding = Encoding.UTF8;
        info.StandardErrorEncoding = Encoding.UTF8;
        info.EnvironmentVariables["STREAMMATE_HOME"] = home;
        Process = new Process();
        Process.StartInfo = info;
        Process.OutputDataReceived += (sender, e) => {
            if (e.Data != null) { Log("stdout " + e.Data); Lines.Enqueue(e.Data); }
        };
        Process.ErrorDataReceived += (sender, e) => {
            if (e.Data != null) Log("stderr " + e.Data);
        };
        Process.Start();
        Process.BeginOutputReadLine();
        Process.BeginErrorReadLine();
    }
    public void Dispose() { if (Process != null) Process.Dispose(); }
}
'@
$runner = New-Object DShowProbeProcess($logPath)
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$homeDir = Join-Path ([IO.Path]::GetTempPath()) ('dshow-proto-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($homeDir)
$script:requestId = 0
$script:currentProps = $null
$script:quit = $false
$exitCode = 0

function Invoke-Rpc([string]$Method, [hashtable]$Params = @{}) {
    $script:requestId++
    $id = $script:requestId
    $request = @{ jsonrpc = '2.0'; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Depth 64 -Compress
    $runner.Log('request ' + $request)
    $bytes = [Text.Encoding]::UTF8.GetBytes($request)
    $segment = New-Object 'System.ArraySegment[byte]' -ArgumentList (,$bytes)
    $timeout = New-Object System.Threading.CancellationTokenSource
    $timeout.CancelAfter(15000)
    try {
        $ws.SendAsync($segment, [Net.WebSockets.WebSocketMessageType]::Text, $true, $timeout.Token).GetAwaiter().GetResult()
        while ($true) {
            $stream = New-Object IO.MemoryStream
            try {
                do {
                    $buffer = New-Object byte[] 8192
                    $chunk = New-Object 'System.ArraySegment[byte]' -ArgumentList (,$buffer)
                    $received = $ws.ReceiveAsync($chunk, $timeout.Token).GetAwaiter().GetResult()
                    if ($received.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { throw 'Host closed the websocket' }
                    $stream.Write($buffer, 0, $received.Count)
                } while (-not $received.EndOfMessage)
                $text = [Text.Encoding]::UTF8.GetString($stream.ToArray())
            } finally { $stream.Dispose() }
            $runner.Log('response/event ' + $text)
            $response = $text | ConvertFrom-Json
            if ($response.id -ne $id) { continue }
            Write-Host ($response | ConvertTo-Json -Depth 64)
            if ($null -ne $response.error) { throw ('RPC error: ' + $text) }
            return $response.result
        }
    } finally { $timeout.Dispose() }
}

function Show-Props($Props) {
    $script:currentProps = $Props
    Write-Host ('Capture size: {0} x {1}; OBS active: {2}' -f $Props.width, $Props.height, $Props.active)
    Write-Host ('Current resolution: {0}; FPS interval: {1}; format: {2}; resolution mode: {3}' -f $Props.settings.resolution, $Props.settings.frame_interval, $Props.settings.video_format, $Props.settings.res_type)
    Write-Host 'Device indices:'
    $devices = @(($Props.properties | Where-Object { $_.name -eq 'video_device_id' }).items)
    for ($i = 0; $i -lt $devices.Count; $i++) {
        Write-Host ('  {0}: {1} | value={2}' -f $i, $devices[$i].name, $devices[$i].value)
    }
    if ($devices.Count -eq 0) { Write-Host '  No devices enumerated. Expected on CI.' }
}

function Invoke-ProbeCommand([string]$Command) {
    $runner.Log('command ' + $Command)
    $parts = $Command.Trim() -split '\s+', 2
    switch ($parts[0].ToLowerInvariant()) {
        'ui' { Invoke-ProbeUi $parts[1] }
        'custom' {
            Show-Props (Invoke-Rpc 'proto.update' @{sourceId='cam';settings=@{res_type=1;resolution='640x480';frame_interval=333333;video_format=0}})
            Start-Sleep -Seconds 2
            Show-Props (Invoke-Rpc 'proto.props' @{sourceId='cam'})
        }
        'props' { Show-Props (Invoke-Rpc 'proto.props' @{ sourceId = 'cam' }) }
        'device' {
            if ($parts.Count -ne 2) { throw 'Use device <index>' }
            $index = [int]$parts[1]
            $devices = @(($script:currentProps.properties | Where-Object { $_.name -eq 'video_device_id' }).items)
            if ($index -lt 0 -or $index -ge $devices.Count) { throw 'Device index out of range' }
            Show-Props (Invoke-Rpc 'proto.update' @{ sourceId = 'cam'; settings = @{ video_device_id = $devices[$index].value } })
            Start-Sleep -Seconds 2
            Show-Props (Invoke-Rpc 'proto.props' @{ sourceId = 'cam' })
        }
        'press' {
            if ($parts.Count -ne 2) { throw 'Use press <button name>' }
            $pressed = Invoke-Rpc 'proto.press' @{ sourceId = 'cam'; property = $parts[1] }
            Write-Host ('Press thread: {0}; main thread: {1}; callback returned: {2}' -f $pressed.hostThreadId, $pressed.mainThreadId, $pressed.callbackReturned)
            Write-Host 'Did the driver dialog appear? Move it, change a value, click OK/Cancel, then run `health` and `props`.'
        }
        'health' { $null = Invoke-Rpc 'host.health' }
        'quit' {
            $null = Invoke-Rpc 'host.shutdown'
            if (-not $runner.Process.WaitForExit(15000)) { throw 'Host did not exit in 15 seconds. Was a driver dialog left open?' }
            $runner.Process.WaitForExit() # Flush asynchronous stdout/stderr callbacks.
            $code = $runner.Process.ExitCode
            Write-Host ('Host exit code: ' + $code)
            $runner.Log('Host exit code: ' + $code)
            $script:quit = $true
            if ($code -ne 0) { throw ('Host failed with exit code ' + $code) }
        }
        default { throw 'Commands: props, device <index>, press <button>, health, quit' }
    }
}

try {
    $exe = (Resolve-Path -LiteralPath $HostExe).Path
    $token = [Guid]::NewGuid().ToString('N')
    Write-Host ('Log: ' + $logPath)
    $runner.Start($exe, $token, $homeDir)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $port = 0
    while ($port -eq 0) {
        $line = ''
        while ($runner.Lines.TryDequeue([ref]$line)) {
            try { $event = $line | ConvertFrom-Json } catch { continue }
            if ($event.event -eq 'host.ready') { $port = [int]$event.port; break }
        }
        if ($runner.Process.HasExited) { throw ('Host exited before ready: ' + $runner.Process.ExitCode) }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Timed out waiting for host.ready' }
        if ($port -eq 0) { Start-Sleep -Milliseconds 100 }
    }
    $ws.Options.SetRequestHeader('Authorization', 'Bearer ' + $token)
    $connectTimeout = New-Object System.Threading.CancellationTokenSource
    $connectTimeout.CancelAfter(15000)
    try { $ws.ConnectAsync([Uri]('ws://127.0.0.1:' + $port + '/control'), $connectTimeout.Token).GetAwaiter().GetResult() }
    finally { $connectTimeout.Dispose() }
    Show-Props (Invoke-Rpc 'proto.createInput' @{ sourceId = 'cam'; kind = 'dshow_input' })
    if ($Script) {
        foreach ($command in ($Script -split ';')) {
            if ($command.Trim()) { Invoke-ProbeCommand $command }
            if ($script:quit) { break }
        }
        if (-not $script:quit) { Invoke-ProbeCommand 'quit' }
    } else {
        Write-Host 'Commands: props | device <index> | press video_config | press xbar_config | press <button> | health | quit'
        while (-not $script:quit) {
            $script:commandIndex++
            $commandFile=Join-Path $proofRoot ('commands/{0:D3}.txt' -f $script:commandIndex)
            $deadline=[DateTime]::UtcNow.AddMinutes(10)
            while (!(Test-Path $commandFile)) {
                if ([DateTime]::UtcNow -gt $deadline) {throw 'Command queue timed out'}
                Start-Sleep -Milliseconds 200
            }
            $command=(Get-Content -Raw $commandFile).Trim()
            $runner.Log('queue ' + $script:commandIndex + ' ' + $command)

            if (-not $command.Trim()) { continue }
            try { Invoke-ProbeCommand $command }
            catch {
                $runner.Log('command error ' + $_.ToString())
                Write-Host $_ -ForegroundColor Red
                if ($runner.Process.HasExited) { throw }
            }
        }
    }
} catch {
    $exitCode = 1
    $runner.Log('ERROR ' + $_.ToString())
    Write-Host $_ -ForegroundColor Red
} finally {
    $ws.Dispose()
    if ($null -ne $runner.Process) {
        if (-not $runner.Process.HasExited) {
            $runner.Log('Cleanup: terminating host after incomplete session')
            $runner.Process.Kill()
        }
        $runner.Process.WaitForExit()
        $runner.Log('Final host exit code: ' + $runner.Process.ExitCode)
    }
    $runner.Dispose()
    Remove-Item -LiteralPath $homeDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ('Send back log: ' + $logPath)
}
exit $exitCode
