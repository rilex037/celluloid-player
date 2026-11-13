# CelluloidPlayer - Makefile (MinGW-w64, FFmpeg dev files in ./ffmpeg)
#   make                                  (on Windows)
#   make CROSS=x86_64-w64-mingw32-        (cross build from Linux)
CROSS   ?=
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
FFMPEG  := ffmpeg
CFLAGS  := -O2 -municode -mwindows -Wall -I$(FFMPEG)/include
SRC     := src/ff.c src/ui_core.c src/ui_draw.c
RES     := app_res.o
HDR     := src/player.h

LIBS := $(FFMPEG)/lib/avformat.lib $(FFMPEG)/lib/avcodec.lib \
        $(FFMPEG)/lib/swscale.lib $(FFMPEG)/lib/swresample.lib \
        $(FFMPEG)/lib/avutil.lib \
        -lws2_32 -lwinmm -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 -ladvapi32 -lgdiplus -ldwmapi

$(RES): assets/app.rc assets/video.ico assets/music.ico
	$(WINDRES) assets/app.rc -O coff -o $@

CelluloidPlayer.exe: $(SRC) $(HDR) $(RES)
	$(CC) $(CFLAGS) $(SRC) $(RES) -o $@ $(LIBS)

.PHONY: clean
clean:
	rm -f CelluloidPlayer.exe $(RES)
