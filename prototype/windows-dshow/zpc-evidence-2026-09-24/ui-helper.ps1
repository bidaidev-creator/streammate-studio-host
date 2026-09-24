# Throwaway native-dialog observer. Only inspects windows owned by this probe's host.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
public class ProbeWindow {
 public long handle; public uint threadId; public string title; public string className;
 public int left,top,width,height,id; public bool enabled;
}
public static class ProbeUi {
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [StructLayout(LayoutKind.Sequential)] public struct RECT {public int L,T,R,B;}
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr p);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool repaint);
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 static ProbeWindow Describe(IntPtr h) {
  uint p; uint t=GetWindowThreadProcessId(h,out p); RECT r; GetWindowRect(h,out r);
  var title=new StringBuilder(512);GetWindowText(h,title,512);var cls=new StringBuilder(128);GetClassName(h,cls,128);
  return new ProbeWindow {handle=h.ToInt64(),threadId=t,title=title.ToString(),className=cls.ToString(),left=r.L,top=r.T,width=r.R-r.L,height=r.B-r.T,id=GetDlgCtrlID(h),enabled=IsWindowEnabled(h)};
 }
 public static ProbeWindow[] Windows(uint pid) {
  var a=new List<ProbeWindow>();EnumWindows((h,p)=>{uint owner;GetWindowThreadProcessId(h,out owner);if(owner==pid && IsWindowVisible(h)) a.Add(Describe(h));return true;},IntPtr.Zero);return a.ToArray();
 }
 public static ProbeWindow[] Children(long h) {
  var a=new List<ProbeWindow>();EnumChildWindows(new IntPtr(h),(c,p)=>{a.Add(Describe(c));return true;},IntPtr.Zero);return a.ToArray();
 }
 public static void Capture(long handle,string path) {
  var h=new IntPtr(handle);RECT r;GetWindowRect(h,out r);
  using(var b=new Bitmap(r.R-r.L,r.B-r.T)) {using(var g=Graphics.FromImage(b)){var dc=g.GetHdc();try{if(!PrintWindow(h,dc,0))throw new Exception("PrintWindow failed");}finally{g.ReleaseHdc(dc);}}b.Save(path,ImageFormat.Png);}
 }
}
'@
function Invoke-ProbeUi([string]$operation) {
 $windows=@([ProbeUi]::Windows([uint32]$runner.Process.Id))
 $runner.Log('UI ' + $operation + ' windows=' + (ConvertTo-Json -InputObject $windows -Depth 5 -Compress))
 if ($operation -eq 'inspect') { return }
 $dialogs=@($windows | Where-Object {$_.className -eq '#32770'})
 if ($dialogs.Count -ne 1) { throw "Expected one host dialog, got $($dialogs.Count)" }
 $dialog=$dialogs[0]; $h=[IntPtr]$dialog.handle
 $children=@([ProbeUi]::Children($dialog.handle))
 $runner.Log('UI children='+(ConvertTo-Json -InputObject $children -Depth 5 -Compress))
 switch -Regex ($operation) {
  '^capture (.+)$' { [ProbeUi]::Capture($dialog.handle,(Join-Path $proofRoot $Matches[1])); break }
  '^move$' { if (-not [ProbeUi]::MoveWindow($h,($dialog.left+80),($dialog.top+60),$dialog.width,$dialog.height,$true)) {throw 'MoveWindow failed'}; break }
  '^change$' {
   $slider=@($children | Where-Object {$_.className -eq 'msctls_trackbar32' -and $_.enabled})[0]
   if (!$slider) {throw 'No enabled native slider'}
   $s=[IntPtr]$slider.handle
   $before=[ProbeUi]::SendMessage($s,0x0400,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64()
   [void][ProbeUi]::SendMessage($s,0x0100,[IntPtr]0x27,[IntPtr]::Zero)
   [void][ProbeUi]::SendMessage($s,0x0101,[IntPtr]0x27,[IntPtr]::Zero)
   $after=[ProbeUi]::SendMessage($s,0x0400,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64()
   $runner.Log("UI slider id=$($slider.id) before=$before after=$after")
   if ($before -eq $after) {throw 'Slider did not change'}
   break
  }
  '^close$' {
   $cancel=@($children | Where-Object {$_.className -eq 'Button' -and $_.id -eq 2})[0]
   if (!$cancel) {throw 'No Cancel button'}
   [void][ProbeUi]::PostMessage([IntPtr]$cancel.handle,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)
   break
  }
  default { throw "Unknown UI operation $operation" }
 }
}
