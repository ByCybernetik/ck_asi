cc = meson.get_compiler('c')

# --- Common flags --------------------------------------------------------

win_defs_xp  = ['-DWINVER=0x0501', '-D_WIN32_WINNT=0x0501']

c_warn   = ['-Wno-cast-function-type', '-Wno-unused-function']

# Vulkan headers from third_party (system fallback via -idirafter)
vk_inc = include_directories('../third_party/Vulkan-Headers/include')
vk_flags = ['-idirafter', '/usr/include']

if not gpu_scene
  add_project_arguments('-DNO_GPU_SCENE', language : ['c', 'cpp'])
endif

# --- FreeType -------------------------------------------------------------
# Always build statically from third_party — no libfreetype-6.dll at runtime.

ft_sub = subproject('freetype2', default_options : ['default_library=static'])
ft_dep = ft_sub.get_variable('freetype_dep')

# --- Include directories -------------------------------------------------

src_inc = include_directories('.')

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
  'audio_wasapi.c',
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
  include_directories : src_inc,
  dependencies : [ft_dep],
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

# --- Native menu stub (no external menu/native_src dependency) -----------

special_libs += static_library('ck_menu_stub',
  'menu/ck_menu_stub.c',
  c_args : win_defs_xp + c_warn,
  include_directories : include_directories('menu'),
)

# --- Win32 link libraries ------------------------------------------------

win_libs = []
foreach lib_name : ['kernel32', 'user32', 'gdi32', 'ole32', 'dsound',
                     'strmiids', 'oleaut32', 'winmm', 'ws2_32']
  win_libs += cc.find_library(lib_name)
endforeach
win_libs += cc.find_library('m')

# --- CK.asi (shared library) --------------------------------------------

asi_inc = [src_inc]
if gpu_scene
  asi_inc += vk_inc
endif

shared_library('CK',
  asi_normal_src,
  name_prefix : '',
  name_suffix : 'asi',
  c_args : win_defs_xp + c_warn,
  include_directories : asi_inc,
  link_with : special_libs,
  dependencies : win_libs,
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
