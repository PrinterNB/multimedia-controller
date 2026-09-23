#Requires AutoHotkey v2.0
#SingleInstance Force

; ------------------------------------------------------------------
; F13 macro launcher for the ESP32-S3 media controller.
;
; The ESP32 sends F13 as a real keyboard key (HID keyboard report).
; This script catches it and launches the target executable.
;
; Run at logon with the script pinned to the tray (default).
; ------------------------------------------------------------------

TargetExe := "C:\Tools\MyApp\myapp.exe"
; false = F13 is a no-op while the app is already running (launch-once)
; true  = F13 restores/minimizes the existing window instead
MinimizeIfRunning := false

~$F13:: {
    if WinExist("ahk_exe " TargetExe) {
        if MinimizeIfRunning {
            WinMinimize("ahk_exe " TargetExe)
            WinActivate("ahk_exe " TargetExe)
        }
        ; else: do nothing — app is already open
        return
    }
    Run TargetExe
    return
}
