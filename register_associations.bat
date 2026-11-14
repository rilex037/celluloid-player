@echo off
rem ============================================================================
rem CelluloidPlayer - Register File Type Associations (Windows)
rem Associates media file extensions with player icons from designated exe path
rem ============================================================================

rem --- Check for admin privileges ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo ============================================================================
    echo ERROR: This script requires administrator privileges to modify the registry.
    echo Please right-click this file and select "Run as administrator", or:
    echo   Right-click -> Properties -> Unblock -> Run as administrator
    echo ============================================================================
    pause
    exit /b 1
)

rem --- Determine player exe path ---
rem Priority: command-line arg > environment variable > default location
if "%~1" neq "" (
    set PLAYER_EXE="%~f1"
    echo Using player exe from command line: %PLAYER_EXE%
) else if defined PLAYER_EXE (
    rem Already set (e.g. from previous run or environment)
    set PLAYER_EXE="%PLAYER_EXE%"
    echo Using player exe from environment: %PLAYER_EXE%
) else (
    set PLAYER_EXE="F:\Programs\bin\CelluloidPlayer.exe"
    echo Using default player exe: %PLAYER_EXE%
    rem Note: User can override by passing path as argument or setting PLAYER_EXE env var
)

rem --- Build icon paths from EXE resources ---
rem The icons are resource IDs 1 (video) and 2 (music) embedded in the EXE
rem Windows ::Icon syntax: "path,-id" where id is the resource ID
set VIDEO_ICON=%PLAYER_EXE%,-1
set MUSIC_ICON=%PLAYER_EXE%,-2

rem --- Register file types ---
echo.
echo ============================================================================
echo Registering CelluloidPlayer file type associations...
echo ============================================================================

rem --- Helper: register a single file type ---
rem %1 = file extension (e.g. .mp3)
rem %2 = icon path (e.g. "C:\Path\To\CelluloidPlayer.exe,-2")
setlocal enabledelayedexpansion
set "EXT=%~1"
set "ICON=%~2"

rem Normalize extension - ensure leading dot
if not "!EXT:~0,1!"=="." set "EXT=!EXT."

rem Remove any existing association for this extension (clean install)
reg delete "HKCR\!EXT%" /f /q 2>nul

rem Create the file type class root key
reg add "HKCR\!EXT%" /f /t REG_SZ /v "" /d "CelluloidPlayerFile" >nul

rem Set the default icon
reg add "HKCR\!EXT%\DefaultIcon" /f /t REG_SZ /v "" /d "!ICON!" >nul

rem Add shell open command to launch the player with the file
reg add "HKCR\!EXT%\shell\open\command" /f /t REG_SZ /v "" "!PLAYER_EXE!" "%%1" >nul

rem Set friendly name displayed in file type info
reg add "HKCR\!EXT%" /f /t REG_SZ /v "FriendlyTypeName" /d "CelluloidPlayer File" >nul

endlocal & goto :eof

rem --- Register .mp3 (music) ---
call :register ".mp3" "%MUSIC_ICON%"

rem --- Register video extensions ---
call :register ".mp4" "%VIDEO_ICON%"
call :register ".avi" "%VIDEO_ICON%"
call :register ".mkv" "%VIDEO_ICON%"
call :register ".webm" "%VIDEO_ICON%"
call :register ".mov" "%VIDEO_ICON%"
call :register ".wmv" "%VIDEO_ICON%"
call :register ".ts" "%VIDEO_ICON%"
call :register ".m2ts" "%VIDEO_ICON%"
call :register ".flv" "%VIDEO_ICON%"
call :register ".ogv" "%VIDEO_ICON%"

rem --- Register audio extensions (additional) ---
call :register ".wav" "%MUSIC_ICON%"
call :register ".flac" "%MUSIC_ICON%"
call :register ".m4a" "%MUSIC_ICON%"
call :register ".aac" "%MUSIC_ICON%"
call :register ".opus" "%MUSIC_ICON%"
call :register ".ogg" "%MUSIC_ICON%"
call :register ".wma" "%MUSIC_ICON%"

rem --- Post-registration summary ---
echo.
echo ============================================================================
echo Registration complete!
echo ============================================================================
echo.
echo Player exe: %PLAYER_EXE%
echo Video icon (IDI_VIDEO): %VIDEO_ICON%
echo Music icon (IDI_MUSIC): %MUSIC_ICON%
echo.
echo Registered extensions:
echo   .mp3, .wav, .flac, .m4a, .aac, .opus, .ogg, .wma  -> Music icon
echo   .mp4, .avi, .mkv, .webm, .mov, .wmv, .ts, .m2ts, .flv, .ogv -> Video icon
echo.
echo To change player location, rerun with path: register_associations.bat "new\path\CelluloidPlayer.exe"
echo.
pause

rem Clean up
endlocal