CC      := i686-w64-mingw32-gcc
CXX     := i686-w64-mingw32-g++
AS      := i686-w64-mingw32-gcc
STRIP   := i686-w64-mingw32-strip

SRCDIR  := src

CFLAGS  := -O2 -Wall -Wextra -Wno-cast-function-type -Wno-unused-function \
           -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -mcrtdll=msvcrt-os \
           $(NO_GPU_SCENE_FLAG)
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 \
           -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
           -fno-exceptions -fno-rtti \
           $(NO_GPU_SCENE_FLAG)
VK_CFLAGS := -idirafter /usr/include
LDFLAGS := -shared -static-libgcc -static-libstdc++ \
           -Wl,--kill-at -Wl,--enable-stdcall-fix \
           -lkernel32 -luser32 -lgdi32 -lole32 -ldsound -lstrmiids -loleaut32 \
           -lwinmm -lws2_32
LDFLAGS_STATIC_PTHREAD := -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic

FT_DIR     := deps/freetype
FT_CFLAGS  := -I$(FT_DIR)/include
FT_LIBS    := -L$(FT_DIR)/lib -lfreetype

GPU_SCENE ?= 1

GAME_DIR ?= ../Imperivm
GAME_DIR_I2 ?= ../Imperivm 2
MENU_INC   := -I$(SRCDIR)/menu -I$(SRCDIR)/menu/iconv_compat \
              -I$(SRCDIR)/menu/native_src -I$(SRCDIR)/menu/native_tp \
              -I$(SRCDIR)/menu/native_tp/stb \
              -include $(SRCDIR)/menu/ck_sdl_stub.h

ASI     := CK.asi
LOADER  := winmm.dll

MENU_OBJS := $(SRCDIR)/menu/ck_menu_host.o \
             $(SRCDIR)/menu/nm_main_menu.o $(SRCDIR)/menu/nm_blit.o \
             $(SRCDIR)/menu/nm_font_stb.o $(SRCDIR)/menu/nm_loc_xml.o \
             $(SRCDIR)/menu/nm_hmmsys.o $(SRCDIR)/menu/nm_lzis.o \
             $(SRCDIR)/menu/nm_menu_widgets.o $(SRCDIR)/menu/nm_load_game_menu.o \
             $(SRCDIR)/menu/nm_ini_file.o $(SRCDIR)/menu/nm_dialog.o \
             $(SRCDIR)/menu/iconv_stub.o

GPU_SCENE_OBJS := $(SRCDIR)/ktx_gpu_terrain.o $(SRCDIR)/vk_terrain.o \
                  $(SRCDIR)/vk_iso_depth.o $(SRCDIR)/vk_decor.o \
                  $(SRCDIR)/vk_obj.o $(SRCDIR)/vk_soft_overlay.o

ifeq ($(GPU_SCENE),0)
  NO_GPU_SCENE_FLAG := -DNO_GPU_SCENE
  GPU_SCENE_OBJS :=
endif

ASI_OBJS := $(SRCDIR)/dllmain.o $(SRCDIR)/log.o $(SRCDIR)/hooks.o \
            $(SRCDIR)/hooks_patch.o $(SRCDIR)/hooks_util.o $(SRCDIR)/hooks_scanline.o \
            $(SRCDIR)/hooks_vfs_map.o $(SRCDIR)/hooks_hitch.o $(SRCDIR)/hooks_terrain.o \
            $(SRCDIR)/hooks_minimap.o $(SRCDIR)/hooks_zoom.o $(SRCDIR)/hooks_cam_smooth.o \
            $(SRCDIR)/hooks_player_color.o $(SRCDIR)/hooks_video.o \
            $(SRCDIR)/hooks_native_menu.o $(SRCDIR)/hooks_obj.o \
            $(SRCDIR)/ktx_terrain.o $(SRCDIR)/ktx_decor.o $(SRCDIR)/decor_spawn.o \
            $(SRCDIR)/ktx_obj.o $(SRCDIR)/obj_spawn.o $(SRCDIR)/obj_player.o \
            $(SRCDIR)/ktx_vq_replace.o $(GPU_SCENE_OBJS) \
            $(SRCDIR)/vk_present.o $(SRCDIR)/tip_font.o \
            $(SRCDIR)/movie_player.o $(SRCDIR)/movie_webm.o $(SRCDIR)/hitch.o \
            $(SRCDIR)/dm_native.o $(SRCDIR)/dm_trace.o $(SRCDIR)/stb_vorbis.o \
            $(SRCDIR)/dm_replace.o $(SRCDIR)/dm_guids.o $(SRCDIR)/dm_globals.o \
            $(SRCDIR)/dm_debug.o $(SRCDIR)/dm_pcm.o \
            $(SRCDIR)/dm_voice.o $(SRCDIR)/dm_assets.o $(SRCDIR)/dm_com.o \
            $(MENU_OBJS)
LDR_OBJS := $(SRCDIR)/loader/dllmain.o $(SRCDIR)/loader/proxy.o \
            $(SRCDIR)/loader/trampolines.o

.PHONY: all clean install install-i2 asi loader

all: $(ASI) $(LOADER)

asi: $(ASI)
loader: $(LOADER)

$(ASI): $(ASI_OBJS)
	$(CXX) $(CFLAGS) -o $@ $(ASI_OBJS) $(LDFLAGS) $(FT_LIBS) -lm $(LDFLAGS_STATIC_PTHREAD)
	$(STRIP) $@
	@echo "built $@"
	@if i686-w64-mingw32-objdump -p $@ | rg -qi 'libgcc|libstdc|libwinpthread|api-ms-win'; then \
		echo "ERROR: MinGW runtime DLL import left in $@"; \
		i686-w64-mingw32-objdump -p $@ | rg -i 'DLL Name:'; \
		exit 1; \
	fi

$(SRCDIR)/stb_vorbis.o: $(SRCDIR)/stb_vorbis.c
	$(CC) $(CFLAGS) -DSTB_VORBIS_NO_STDIO -Wno-unused-parameter -Wno-unused-value \
		-Wno-maybe-uninitialized -c -o $@ $<

$(SRCDIR)/tip_font.o: $(SRCDIR)/tip_font.c $(SRCDIR)/tip_font.h
	$(CC) $(CFLAGS) $(FT_CFLAGS) -fno-ipa-cp-clone -c -o $@ $<

$(SRCDIR)/menu/ck_menu_host.o: $(SRCDIR)/menu/ck_menu_host.cpp $(SRCDIR)/menu/ck_menu.h $(SRCDIR)/menu/ck_sdl_stub.h
	$(CXX) $(CXXFLAGS) $(MENU_INC) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/menu/nm_main_menu.o: $(SRCDIR)/menu/native_src/ui/main_menu.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_blit.o: $(SRCDIR)/menu/native_src/ui/blit.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_font_stb.o: $(SRCDIR)/menu/native_src/ui/font_stb.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_loc_xml.o: $(SRCDIR)/menu/native_src/ui/loc_xml.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_hmmsys.o: $(SRCDIR)/menu/native_src/vfs/hmmsys.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_lzis.o: $(SRCDIR)/menu/native_src/vfs/lzis.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_menu_widgets.o: $(SRCDIR)/menu/native_src/ui/menu_widgets.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_load_game_menu.o: $(SRCDIR)/menu/native_src/ui/load_game_menu.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_ini_file.o: $(SRCDIR)/menu/native_src/ui/ini_file.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/nm_dialog.o: $(SRCDIR)/menu/native_src/ui/dialog.cpp
	$(CXX) $(filter-out -fno-exceptions,$(CXXFLAGS)) -fexceptions $(MENU_INC) -c -o $@ $<

$(SRCDIR)/menu/iconv_stub.o: $(SRCDIR)/menu/iconv_compat/iconv_stub.c $(SRCDIR)/menu/iconv_compat/iconv.h
	$(CC) $(CFLAGS) -I$(SRCDIR)/menu/iconv_compat -c -o $@ $<

$(LOADER): $(LDR_OBJS) $(SRCDIR)/loader/winmm.def
	$(CC) $(CFLAGS) -o $@ $(LDR_OBJS) $(SRCDIR)/loader/winmm.def \
		-shared -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fix -lkernel32 -luser32
	$(STRIP) $@
	@echo "built $@"

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/vk_present.o: $(SRCDIR)/vk_present.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/vk_terrain.o: $(SRCDIR)/vk_terrain.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/vk_iso_depth.o: $(SRCDIR)/vk_iso_depth.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/vk_decor.o: $(SRCDIR)/vk_decor.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/vk_obj.o: $(SRCDIR)/vk_obj.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/vk_soft_overlay.o: $(SRCDIR)/vk_soft_overlay.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(SRCDIR)/loader/dllmain.o: $(SRCDIR)/loader/dllmain.c
	$(CC) $(CFLAGS) -I$(SRCDIR)/loader -c -o $@ $<

$(SRCDIR)/loader/proxy.o: $(SRCDIR)/loader/proxy.c
	$(CC) $(CFLAGS) -I$(SRCDIR)/loader -c -o $@ $<

$(SRCDIR)/loader/trampolines.o: $(SRCDIR)/loader/trampolines.S
	$(AS) -c -o $@ $<

clean:
	rm -f $(ASI_OBJS) $(LDR_OBJS) $(ASI) $(LOADER)

install: all
	mkdir -p "$(GAME_DIR)/scripts"
	cp -f $(ASI) "$(GAME_DIR)/scripts/CK.asi"
	cp -f $(LOADER) "$(GAME_DIR)/winmm.dll"
	@echo "installed $(GAME_DIR)/scripts/CK.asi + $(GAME_DIR)/winmm.dll"
	@echo "  WINEDLLOVERRIDES='winmm=n,b'  CK_DM_REPLACE=1  (loader pulls scripts/CK.asi)"

install-i2: all
	mkdir -p "$(GAME_DIR_I2)/scripts"
	@if [ -L "$(GAME_DIR_I2)/scripts/CK.asi" ] || cmp -s $(ASI) "$(GAME_DIR_I2)/scripts/CK.asi" 2>/dev/null; then \
		echo "CK.asi already at $(GAME_DIR_I2)/scripts/CK.asi"; \
	else \
		cp -f $(ASI) "$(GAME_DIR_I2)/scripts/CK.asi"; \
	fi
	cp -f tahomabd.ttf "$(GAME_DIR_I2)/scripts/tahomabd.ttf" 2>/dev/null || true
	cp -f tahoma.ttf "$(GAME_DIR_I2)/scripts/tahoma.ttf" 2>/dev/null || true
	@if [ -f $(LOADER) ]; then \
	  if cmp -s $(LOADER) "$(GAME_DIR_I2)/winmm.dll" 2>/dev/null; then \
	    echo "winmm.dll up to date"; \
	  else \
	    cp -f $(LOADER) "$(GAME_DIR_I2)/winmm.dll" || true; \
	  fi; \
	fi
	@echo "installed $(GAME_DIR_I2)/scripts/CK.asi + $(GAME_DIR_I2)/winmm.dll"
	@echo "  WINEDLLOVERRIDES='winmm=n,b'  CK_DM_REPLACE=1"
