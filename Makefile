CC      := i686-w64-mingw32-gcc
CXX     := i686-w64-mingw32-g++
AS      := i686-w64-mingw32-gcc
STRIP   := i686-w64-mingw32-strip

CFLAGS  := -O2 -Wall -Wextra -Wno-cast-function-type -Wno-unused-function \
           -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -mcrtdll=msvcrt-os \
           $(NO_GPU_SCENE_FLAG)
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 \
           -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
           -fno-exceptions -fno-rtti \
           $(NO_GPU_SCENE_FLAG)
# Note: no -mcrtdll=msvcrt-os here — old CRT lacks at_quick_exit needed by libstdc++.
VK_CFLAGS := -idirafter /usr/include
LDFLAGS := -shared -static-libgcc -static-libstdc++ \
           -Wl,--kill-at -Wl,--enable-stdcall-fix \
           -lkernel32 -luser32 -lgdi32 -lole32 -ldsound -lstrmiids -loleaut32 \
           -lwinmm -lws2_32
# MinGW must not leave libwinpthread-1.dll / libstdc++-6.dll imports (see mingw-win98 skill).
LDFLAGS_STATIC_PTHREAD := -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic

FT_DIR     := deps/freetype
FT_CFLAGS  := -I$(FT_DIR)/include
FT_LIBS    := -L$(FT_DIR)/lib -lfreetype

GPU_SCENE ?= 1

GAME_DIR ?= ../Imperivm
GAME_DIR_I2 ?= ../Imperivm 2
# Symlinks menu/native_src → Imperivm 2/native/src (avoid spaces in make deps)
# iconv_compat first so <iconv.h> resolves to our stub.
MENU_INC   := -Imenu -Imenu/iconv_compat -Imenu/native_src -Imenu/native_tp -Imenu/native_tp/stb \
              -include menu/ck_sdl_stub.h

ASI     := CK.asi
LOADER  := loader/winmm.dll
WEBMDEC := tools/ck_webm_dec

MENU_OBJS := menu/ck_menu_host.o \
             menu/nm_main_menu.o menu/nm_blit.o menu/nm_font_stb.o menu/nm_loc_xml.o \
             menu/nm_hmmsys.o menu/nm_lzis.o \
             menu/nm_menu_widgets.o menu/nm_load_game_menu.o \
             menu/nm_ini_file.o menu/nm_dialog.o \
             menu/iconv_stub.o

GPU_SCENE_OBJS := ktx_gpu_terrain.o vk_terrain.o vk_iso_depth.o \
                  vk_decor.o vk_obj.o vk_soft_overlay.o

ifeq ($(GPU_SCENE),0)
  NO_GPU_SCENE_FLAG := -DNO_GPU_SCENE
  GPU_SCENE_OBJS :=
endif

ASI_OBJS := dllmain.o log.o hooks.o hooks_patch.o hooks_util.o hooks_scanline.o \
            hooks_vfs_map.o hooks_hitch.o hooks_terrain.o hooks_minimap.o hooks_zoom.o \
            hooks_cam_smooth.o \
            hooks_player_color.o hooks_video.o hooks_native_menu.o hooks_obj.o \
            ktx_terrain.o ktx_decor.o decor_spawn.o ktx_obj.o obj_spawn.o obj_player.o \
            ktx_vq_replace.o $(GPU_SCENE_OBJS) \
            vk_present.o tip_font.o movie_player.o movie_webm.o hitch.o \
            dm_native.o dm_trace.o stb_vorbis.o \
            dm_replace.o dm_guids.o dm_globals.o dm_debug.o dm_pcm.o \
            dm_voice.o dm_assets.o dm_com.o \
            $(MENU_OBJS)
LDR_OBJS := loader/dllmain.o loader/proxy.o loader/trampolines.o

.PHONY: all clean install install-i2 asi loader webmdec

all: $(ASI) $(LOADER) $(WEBMDEC)

asi: $(ASI)
loader: $(LOADER)
webmdec: $(WEBMDEC)

$(WEBMDEC): tools/ck_webm_dec.c
	cc -O2 -Wall -Wextra -o $@ $< $$(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale libswresample)
	@echo "built $@"

$(ASI): $(ASI_OBJS)
	$(CXX) $(CFLAGS) -o $@ $(ASI_OBJS) $(LDFLAGS) $(FT_LIBS) -lm $(LDFLAGS_STATIC_PTHREAD)
	$(STRIP) $@
	@echo "built $@"
	@if i686-w64-mingw32-objdump -p $@ | rg -qi 'libgcc|libstdc|libwinpthread|api-ms-win'; then \
		echo "ERROR: MinGW runtime DLL import left in $@"; \
		i686-w64-mingw32-objdump -p $@ | rg -i 'DLL Name:'; \
		exit 1; \
	fi

stb_vorbis.o: stb_vorbis.c
	$(CC) $(CFLAGS) -DSTB_VORBIS_NO_STDIO -Wno-unused-parameter -Wno-unused-value \
		-Wno-maybe-uninitialized -c -o $@ $<

tip_font.o: tip_font.c tip_font.h
	$(CC) $(CFLAGS) $(FT_CFLAGS) -fno-ipa-cp-clone -c -o $@ tip_font.c

menu/ck_menu_host.o: menu/ck_menu_host.cpp menu/ck_menu.h menu/ck_sdl_stub.h
	$(CXX) $(CXXFLAGS) $(MENU_INC) -I. -c -o $@ menu/ck_menu_host.cpp

menu/nm_main_menu.o: menu/native_src/ui/main_menu.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_blit.o: menu/native_src/ui/blit.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_font_stb.o: menu/native_src/ui/font_stb.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_loc_xml.o: menu/native_src/ui/loc_xml.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_hmmsys.o: menu/native_src/vfs/hmmsys.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_lzis.o: menu/native_src/vfs/lzis.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_menu_widgets.o: menu/native_src/ui/menu_widgets.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_load_game_menu.o: menu/native_src/ui/load_game_menu.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_ini_file.o: menu/native_src/ui/ini_file.cpp
	$(CXX) $(CXXFLAGS) $(MENU_INC) -c -o $@ $<

menu/nm_dialog.o: menu/native_src/ui/dialog.cpp
	$(CXX) $(filter-out -fno-exceptions,$(CXXFLAGS)) -fexceptions $(MENU_INC) -c -o $@ $<

menu/iconv_stub.o: menu/iconv_compat/iconv_stub.c menu/iconv_compat/iconv.h
	$(CC) $(CFLAGS) -Imenu/iconv_compat -c -o $@ menu/iconv_compat/iconv_stub.c

$(LOADER): $(LDR_OBJS) loader/winmm.def
	$(CC) $(CFLAGS) -o $@ $(LDR_OBJS) loader/winmm.def \
		-shared -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fix -lkernel32 -luser32
	$(STRIP) $@
	@echo "built $@"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

vk_present.o: vk_present.c
	$(CC) $(CFLAGS) $(VK_CFLAGS) -c -o $@ $<

vk_terrain.o: vk_terrain.c shaders/terrain_vert_spv.h shaders/terrain_frag_spv.h \
	shaders/water_blend_vert_spv.h shaders/water_blend_frag_spv.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I. -c -o $@ $<

vk_iso_depth.o: vk_iso_depth.c vk_iso_depth.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -c -o $@ $<

vk_decor.o: vk_decor.c shaders/decor_vert_spv.h shaders/decor_frag_spv.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I. -c -o $@ $<

vk_obj.o: vk_obj.c shaders/obj_vert_spv.h shaders/obj_frag_spv.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I. -c -o $@ $<

vk_soft_overlay.o: vk_soft_overlay.c shaders/soft_overlay_vert_spv.h shaders/soft_overlay_frag_spv.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I. -c -o $@ $<

loader/dllmain.o: loader/dllmain.c
	$(CC) $(CFLAGS) -Iloader -c -o $@ $<

loader/proxy.o: loader/proxy.c
	$(CC) $(CFLAGS) -Iloader -c -o $@ $<

loader/trampolines.o: loader/trampolines.S
	$(AS) -c -o $@ $<

clean:
	rm -f $(ASI_OBJS) $(LDR_OBJS) $(ASI) $(LOADER) $(WEBMDEC)

# Install into Celtic Kings game tree
install: all
	mkdir -p "$(GAME_DIR)/scripts"
	cp -f $(ASI) "$(GAME_DIR)/scripts/CK.asi"
	cp -f $(LOADER) "$(GAME_DIR)/winmm.dll"
	@echo "installed $(GAME_DIR)/scripts/CK.asi + $(GAME_DIR)/winmm.dll"
	@echo "  WINEDLLOVERRIDES='winmm=n,b'  CK_DM_REPLACE=1  (loader pulls scripts/CK.asi)"

# Imperivm 2 / tpw.exe — same ASI; SFX from Packs/Sounds.pak
# scripts/CK.asi may be a symlink to this tree — skip copy if identical.
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
