# PROTOTYPE — throwaway, never merge

This checks whether the headless Windows host can open a camera driver's native
DirectShow dialogs. Allow about 10 minutes. CI has no camera and cannot answer
whether a real driver's dialog works.

1. Download the GitHub Actions artifact `streammate-studio-host-windows-x64`.
   Unzip it, then extract the tarball with Windows' built-in tar:
   `tar -xzf StreamMateStudioHost-windows-x64.tar.gz`.
   `dshow-probe.ps1` is beside `studio-host.exe` inside that archive.
2. Plug in a USB webcam. Close other apps that might be using it.
3. Open PowerShell in the extracted bundle folder via the folder's right-click
   menu. Run `powershell -ExecutionPolicy Bypass -File .\dshow-probe.ps1`.
   Record any SmartScreen or Defender prompts for the unsigned `studio-host.exe`.
4. Read the device indices, then enter `device 0`, or the index for your webcam.
   Check the settings and capture width/height printed after two seconds.
5. Enter `press video_config`. Did a driver dialog appear? It has no owner
   window, so if nothing shows, check the taskbar and Alt-Tab before answering
   no, and note where it opened (front, behind, taskbar only). Move it, change a
   setting, and try `health` while it is open. Click OK or Cancel, then run
   `health` and `props` again. Press the button a second time and close it again.
6. Enter `press xbar_config`. Most USB webcams have no crossbar, so no dialog
   may appear. Record what happens. Close any dialog, then run `health`.
7. Enter `quit` with all driver dialogs closed. Check that the host exits with
   code 0. Send back the timestamped `dshow-probe-*.log` beside the script.

Report: dialog appeared yes/no; could interact yes/no; host survived while open
and after closing yes/no; second opening worked yes/no; crossbar behavior;
anything odd; SmartScreen/Defender prompts. The log includes requests, full
responses, thread IDs, device names/settings, and host stdout/stderr. Review it
before sharing if your device names contain private information.

`props` reprints the devices and full properties. `press <button name>` works
for any button in that JSON. `health` checks host readiness. `active` is libobs
scene activation, so it can be false for this unattached camera even while
DirectShow is capturing. `settings.active` is the driver's separate setting.
FPS is exposed as `frame_interval` in 100-nanosecond units; 333333 is about 30 FPS.
Device-default resolution/FPS/format settings can be empty or sentinel values.

For automation with Windows PowerShell 5.1:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\dshow-probe.ps1 -Script "props;press video_config;health;quit"
```

Override the executable with `-HostExe C:\path\studio-host.exe`. Each run uses
one host, one authenticated websocket, and a temporary STREAMMATE_HOME. An RPC
that stalls for 15 seconds fails and is logged; failed sessions terminate the
host. The probe does not create a UI thread or install a virtual camera.
