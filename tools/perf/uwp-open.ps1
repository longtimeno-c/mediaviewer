# Opens a file in a packaged (UWP) app the way "Open with" does: IApplicationActivationManager.ActivateForFile.
# Usage: uwp-open.ps1 -AppId "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic" -Path C:\clip.mp4
param([Parameter(Mandatory)][string]$AppId, [Parameter(Mandatory)][string]$Path)
Add-Type @"
using System;using System.Runtime.InteropServices;
[ComImport,Guid("2e941141-7f97-4756-ba1d-9decde894a3d"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IApplicationActivationManager{
 int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)]string a,[MarshalAs(UnmanagedType.LPWStr)]string b,int o,out uint pid);
 int ActivateForFile([MarshalAs(UnmanagedType.LPWStr)]string a,IntPtr items,[MarshalAs(UnmanagedType.LPWStr)]string verb,out uint pid);
 int ActivateForProtocol([MarshalAs(UnmanagedType.LPWStr)]string a,IntPtr items,out uint pid);}
[ComImport,Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")]public class AAM{}
public static class Act{
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)]static extern int SHCreateItemFromParsingName(string p,IntPtr b,ref Guid r,out IntPtr o);
 [DllImport("shell32.dll")]static extern int SHCreateShellItemArrayFromShellItem(IntPtr i,ref Guid r,out IntPtr o);
 public static uint Open(string app,string path){
  Guid si=new Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"),sia=new Guid("b63ea76d-1f85-456f-a19c-48159efa858b");
  IntPtr item,arr;
  Marshal.ThrowExceptionForHR(SHCreateItemFromParsingName(path,IntPtr.Zero,ref si,out item));
  Marshal.ThrowExceptionForHR(SHCreateShellItemArrayFromShellItem(item,ref sia,out arr));
  uint pid;var m=(IApplicationActivationManager)new AAM();
  Marshal.ThrowExceptionForHR(m.ActivateForFile(app,arr,"open",out pid));return pid;}
}
"@
[Act]::Open($AppId, $Path)
