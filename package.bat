@echo off
setlocal
cd /d "%~dp0"
if not exist build\Ickza.exe (echo build\Ickza.exe missing - run build.bat first & exit /b 1)
for /f "tokens=3 delims= " %%v in ('findstr ICKZA_VERSION src\version.h') do set "VER=%%~v"
set "ZIP=Ickza-%VER%.zip"
set "STAGE=%TEMP%\ickza_pkg\Ickza"
if exist "%TEMP%\ickza_pkg" rmdir /s /q "%TEMP%\ickza_pkg"
robocopy build "%STAGE%" /e /xf Ickza.map Ickza.ini Ickza.log mmap.txt /xd maps /njh /njs /ndl /nfl >nul
copy /y LICENSE "%STAGE%\" >nul
copy /y THIRD_PARTY.md "%STAGE%\" >nul
if exist "%ZIP%" del "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%'" || exit /b 1
rmdir /s /q "%TEMP%\ickza_pkg"
echo packaged %ZIP%
exit /b 0
