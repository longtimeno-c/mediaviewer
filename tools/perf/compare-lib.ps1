# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# Shared helpers for tools/perf/compare.ps1: find a window, capture only that window, drive the mouse.
# Only the target window is ever captured, never the whole desktop.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;using System.Drawing;
public class Win{
 public delegate bool EP(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")]static extern bool EnumWindows(EP e,IntPtr l);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern void mouse_event(uint f,int dx,int dy,int d,IntPtr e);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")]public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int hh,bool r);
 [DllImport("user32.dll")]public static extern void keybd_event(byte k,byte s,uint f,IntPtr e);
 [StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}
 static Win(){SetProcessDPIAware();}
 public static IntPtr Find(string sub){IntPtr f=IntPtr.Zero;EnumWindows((h,l)=>{if(f==IntPtr.Zero&&IsWindowVisible(h)){var s=new StringBuilder(256);GetWindowText(h,s,256);if(s.Length>0&&s.ToString().IndexOf(sub,StringComparison.OrdinalIgnoreCase)>=0)f=h;}return true;},IntPtr.Zero);return f;}
 public static int[] Rect(IntPtr h){RECT r;GetWindowRect(h,out r);return new[]{r.L,r.T,r.R-r.L,r.B-r.T};}
 // 1/8-scale RGB grab of a screen rectangle, used only to time when the picture appears (never saved).
 public static byte[] Grab(int x,int y,int w,int h){
  using(var b=new Bitmap(w,h,System.Drawing.Imaging.PixelFormat.Format24bppRgb)){using(var g=Graphics.FromImage(b))g.CopyFromScreen(x,y,0,0,b.Size);
   int sw=w/8,sh=h/8;using(var s=new Bitmap(sw,sh,System.Drawing.Imaging.PixelFormat.Format24bppRgb)){using(var g=Graphics.FromImage(s)){g.InterpolationMode=System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;g.DrawImage(b,0,0,sw,sh);}
    var bd=s.LockBits(new Rectangle(0,0,sw,sh),System.Drawing.Imaging.ImageLockMode.ReadOnly,System.Drawing.Imaging.PixelFormat.Format24bppRgb);
    var d=new byte[sw*sh*3];for(int yy=0;yy<sh;yy++)Marshal.Copy(bd.Scan0+yy*bd.Stride,d,yy*sw*3,sw*3);s.UnlockBits(bd);return d;}}}
 // Of the pixels that differ between `baseline` and `final`, the fraction of `cur` already within tolerance of `final`.
 public static double Settled(byte[] baseline,byte[] final,byte[] cur){int m=0,ok=0;for(int i=0;i<final.Length;i+=3){int db=Math.Max(Math.Abs(final[i]-baseline[i]),Math.Max(Math.Abs(final[i+1]-baseline[i+1]),Math.Abs(final[i+2]-baseline[i+2])));if(db<=40)continue;m++;int dc=Math.Max(Math.Abs(final[i]-cur[i]),Math.Max(Math.Abs(final[i+1]-cur[i+1]),Math.Abs(final[i+2]-cur[i+2])));if(dc<=40)ok++;}return m==0?0:(double)ok/m;}
 public static void Save(int x,int y,int w,int h,string path){using(var b=new Bitmap(w,h)){using(var g=Graphics.FromImage(b))g.CopyFromScreen(x,y,0,0,b.Size);b.Save(path,System.Drawing.Imaging.ImageFormat.Png);}}
 public static void Wheel(int notches,bool ctrl){if(ctrl)keybd_event(0x11,0,0,IntPtr.Zero);for(int i=0;i<Math.Abs(notches);i++){mouse_event(0x0800,0,0,notches>0?120:-120,IntPtr.Zero);System.Threading.Thread.Sleep(120);}if(ctrl)keybd_event(0x11,0,2,IntPtr.Zero);}
 // Hold the left button and move the cursor on a Lissajous path for `seconds`, one step per ~4 ms.
 public static void Drag(int cx,int cy,int rx,int ry,double seconds){
  SetCursorPos(cx,cy);mouse_event(0x0002,0,0,0,IntPtr.Zero);
  var sw=System.Diagnostics.Stopwatch.StartNew();
  while(sw.Elapsed.TotalSeconds<seconds){double t=sw.Elapsed.TotalSeconds;SetCursorPos(cx+(int)(rx*Math.Sin(t*1.3)),cy+(int)(ry*Math.Sin(t*1.9)));System.Threading.Thread.Sleep(4);}
  mouse_event(0x0004,0,0,0,IntPtr.Zero);}
}
"@ -ReferencedAssemblies System.Drawing
