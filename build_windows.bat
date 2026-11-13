@echo off
rem CelluloidPlayer - build script (Windows, MinGW-w64 + FFmpeg shared dev)
rem Needs MinGW-w64 gcc in PATH and an FFmpeg *shared* build unpacked into .\ffmpeg
rem After building, copy .\ffmpeg\bin\{avcodec,avformat,avutil,swscale,swresample}-*.dll
rem next to CelluloidPlayer.exe.

setlocal
where gcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: gcc not found in PATH.
    exit /b 1
)
if not exist ffmpeg\include\libavformat\avformat.h (
    echo ERROR: FFmpeg dev files not found in .\ffmpeg
    exit /b 1
)

windres assets\app.rc -O coff -o app_res.o
if errorlevel 1 (
    echo RESOURCE BUILD FAILED
    exit /b 1
)

gcc -O2 -municode -mwindows -Wall ^
    -I ffmpeg\include ^
    src\ff.c src\ui_core.c src\ui_draw.c app_res.o ^
    -o CelluloidPlayer.exe ^
    ffmpeg\lib\avformat.lib ffmpeg\lib\avcodec.lib ^
    ffmpeg\lib\swscale.lib ffmpeg\lib\swresample.lib ffmpeg\lib\avutil.lib ^
    -lws2_32 -lwinmm -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 -ladvapi32 -lgdiplus -ldwmapi

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo Built CelluloidPlayer.exe
endlocal
