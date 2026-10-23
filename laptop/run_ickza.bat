@echo off
cd /d "%~dp0"
if exist Ickza_pending.exe move /y Ickza_pending.exe Ickza.exe >nul
start "" Ickza.exe --dma --fullscreen
