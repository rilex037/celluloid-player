@echo off
rem ============================================================================
rem CelluloidPlayer - Unregister File Type Associations (Windows)
rem Removes media file extensions associations from the registry
rem ============================================================================

rem --- Check for admin privileges ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo ============================================================================
    echo ERROR: This script requires administrator privileges to modify the registry.
    echo Please right-click this file and select "Run as administrator".
    echo ============================================================================
    pause
    exit /b 1
)

rem --- Determine which extensions to remove ---
rem By default, remove all extensions we registered
set "EXTENSIONS=mp3 mp4 avi mkv webm mov wmv ts m2ts flv ogv wav flac m4a aac opus ogg wma"

rem --- Helper: unregister a single file type ---
rem %1 = file extension (e.g. .mp3) WITHOUT the leading dot
setlocal enabledelayedexpansion
set "EXT=%~1"

rem Add leading dot if not present
if not "!EXT:~0,1!"=="." set "EXT=!EXT."

rem Remove the entire file type class key
reg delete "HKCR\!EXT%" /f /q 2>nul

endlocal & goto :eof

rem --- Unregister all extensions ---
echo.
echo ============================================================================
echo Removing CelluloidPlayer file type associations...
echo ============================================================================

for %%E in (%EXTENSIONS%) do (
    echo.
    echo Removing .%%E association...
    call :unregister %%E
)

rem --- Post-removal summary ---
echo.
echo ============================================================================
echo Unregistration complete!
echo ============================================================================
echo.
echo The following extensions have been disassociated:
echo   .mp3, .wav, .flac, .m4a, .aac, .opus, .ogg, .wma
echo   .mp4, .avi, .mkv, .webm, .mov, .wmv, .ts, .m2ts, .flv, .ogv
echo.
echo File associations have been removed from the registry.
echo The player exe and its embedded icons are unaffected.
echo.
echo To re-register, run: register_associations.bat
echo.
pause