cc = meson.get_compiler('c')

# --- Common flags --------------------------------------------------------

win_defs_xp  = ['-DWINVER=0x0501', '-D_WIN32_WINNT=0x0501']
win_defs_7   = ['-DWINVER=0x0601', '-D_WIN32_WINNT=0x0601']

c_warn   = ['-Wno-cast-function-type', '-Wno-unused-function']

# Vulkan headers from third_party (system fallback via -idirafter)
vk_inc = include_directories('../third_party/Vulkan-Headers/include')
vk_flags = ['-idirafter', '/usr/include']

if not gpu_scene
  add_project_arguments('-DNO_GPU_SCENE', language : ['c', 'cpp'])
endif

# --- FreeType -------------------------------------------------------------
# Headers from third_party submodule; prebuilt lib from deps/freetype/lib.

ft_inc = include_directories('../third_party/freetype/include')
ft_lib = cc.find_library('freetype', dirs : [
  meson.project_source_root() / 'deps' / 'freetype' / 'lib',
])

# --- Include directories -------------------------------------------------

src_inc = include_directories('.')

menu_inc = include_directories(
  'menu',
  'menu/iconv_compat',
  'menu/native_src',
  'menu/native_tp',
  'menu/native_tp/stb',
)

# --- ASI: normal C sources (default c_args) ------------------------------

asi_normal_src = files(
  'dllmain.c',
  'log.c',
  'hooks.c',
  'hooks_patch.c',
  'hooks_util.c',
  'hooks_scanline.c',
  'hooks_vfs_map.c',
  'hooks_hitch.c',
  'hooks_terrain.c',
  'hooks_minimap.c',
  'hooks_zoom.c',
  'hooks_cam_smooth.c',
  'hooks_player_color.c',
  'hooks_video.c',
  'hooks_native_menu.c',
  'hooks_obj.c',
  'ktx_terrain.c',
  'ktx_decor.c',
  'decor_spawn.c',
  'ktx_obj.c',
  'obj_spawn.c',
  'obj_player.c',
  'ktx_vq_replace.c',
  'movie_player.c',
  'movie_webm.c',
  'hitch.c',
  'dm_native.c',
  'dm_trace.c',
  'dm_replace.c',
  'dm_guids.c',
  'dm_globals.c',
  'dm_debug.c',
  'dm_pcm.c',
  'dm_voice.c',
  'dm_assets.c',
  'dm_com.c',
)

if gpu_scene
  asi_normal_src += files('ktx_gpu_terrain.c')
endif

# --- ASI: files with special compile flags (built as static libs) --------

lib_stb_vorbis = static_library('stb_vorbis', 'stb_vorbis.c',
  c_args : win_defs_xp + c_warn + [
    '-DSTB_VORBIS_NO_STDIO',
    '-Wno-unused-parameter', '-Wno-unused-value', '-Wno-maybe-uninitialized',
  ],
  include_directories : src_inc,
)

lib_tip_font = static_library('tip_font', 'tip_font.c',
  c_args : win_defs_xp + c_warn + ['-fno-ipa-cp-clone'],
  include_directories : [src_inc, ft_inc],
)

vk_c_args = win_defs_xp + c_warn + vk_flags

lib_vk_present = static_library('vk_present', 'vk_present.c',
  c_args : vk_c_args,
  include_directories : [src_inc, vk_inc],
)

special_libs = [lib_stb_vorbis, lib_tip_font, lib_vk_present]

if gpu_scene
  special_libs += static_library('vk_terrain', 'vk_terrain.c',
    c_args : vk_c_args,
    include_directories : [src_inc, vk_inc],
  )
  special_libs += static_library('vk_iso_depth', 'vk_iso_depth.c',
    c_args : vk_c_args,
    include_directories : [src_inc, vk_inc],
  )
  special_libs += static_library('vk_decor', 'vk_decor.c',
    c_args : vk_c_args,
    include_directories : [src_inc, vk_inc],
  )
  special_libs += static_library('vk_obj', 'vk_obj.c',
    c_args : vk_c_args,
    include_directories : [src_inc, vk_inc],
  )
  special_libs += static_library('vk_soft_overlay', 'vk_soft_overlay.c',
    c_args : vk_c_args,
    include_directories : [src_inc, vk_inc],
  )
endif

# --- Menu (C++) ----------------------------------------------------------

menu_force_inc = meson.current_source_dir() / 'menu' / 'ck_sdl_stub.h'
menu_cpp_args = win_defs_7 + ['-include', menu_force_inc]

special_libs += static_library('ck_menu_host', 'menu/ck_menu_host.cpp',
  cpp_args : menu_cpp_args,
  include_directories : [src_inc, menu_inc],
)

menu_native_sources = {
  'nm_main_menu'     : 'menu/native_src/ui/main_menu.cpp',
  'nm_blit'          : 'menu/native_src/ui/blit.cpp',
  'nm_font_stb'      : 'menu/native_src/ui/font_stb.cpp',
  'nm_loc_xml'       : 'menu/native_src/ui/loc_xml.cpp',
  'nm_hmmsys'        : 'menu/native_src/vfs/hmmsys.cpp',
  'nm_lzis'          : 'menu/native_src/vfs/lzis.cpp',
  'nm_menu_widgets'  : 'menu/native_src/ui/menu_widgets.cpp',
  'nm_load_game_menu': 'menu/native_src/ui/load_game_menu.cpp',
  'nm_ini_file'      : 'menu/native_src/ui/ini_file.cpp',
}

foreach name, src : menu_native_sources
  special_libs += static_library(name, src,
    cpp_args : menu_cpp_args,
    include_directories : menu_inc,
  )
endforeach

special_libs += static_library('nm_dialog',
  'menu/native_src/ui/dialog.cpp',
  cpp_args : menu_cpp_args + ['-fexceptions'],
  override_options : ['cpp_eh=default'],
  include_directories : menu_inc,
)

special_libs += static_library('iconv_stub',
  'menu/iconv_compat/iconv_stub.c',
  c_args : win_defs_xp + c_warn,
  include_directories : include_directories('menu/iconv_compat'),
)

# --- Win32 link libraries ------------------------------------------------

win_libs = []
foreach lib_name : ['kernel32', 'user32', 'gdi32', 'ole32', 'dsound',
                     'strmiids', 'oleaut32', 'winmm', 'ws2_32']
  win_libs += cc.find_library(lib_name)
endforeach
win_libs += cc.find_library('m')

# --- CK.asi (shared library) --------------------------------------------

shared_library('CK',
  asi_normal_src,
  name_prefix : '',
  name_suffix : 'asi',
  c_args : win_defs_xp + c_warn,
  include_directories : src_inc,
  link_with : special_libs,
  dependencies : win_libs + [ft_lib],
  link_args : ['-Wl,--kill-at', '-Wl,--enable-stdcall-fix'],
)

# --- Loader (winmm.dll) --------------------------------------------------

shared_library('winmm',
  ['loader/dllmain.c', 'loader/proxy.c', 'loader/trampolines.S'],
  name_prefix : '',
  c_args : win_defs_xp + c_warn,
  include_directories : include_directories('loader'),
  vs_module_defs : 'loader/winmm.def',
  link_args : ['-static-libgcc', '-Wl,--kill-at', '-Wl,--enable-stdcall-fix'],
  dependencies : [
    cc.find_library('kernel32'),
    cc.find_library('user32'),
  ],
)
