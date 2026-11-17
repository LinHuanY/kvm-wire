@echo off
chcp 65001 >nul
echo 正在请求管理员权限...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-NoExit','-ExecutionPolicy','Bypass','-File','K:\00_CY\05_ipkvm\opensource\packaging\install_service.ps1'"
echo.
echo 如果弹出 UAC，请点“是”；安装窗口会保留显示结果。
pause
