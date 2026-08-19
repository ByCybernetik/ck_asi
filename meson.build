project('ck_asi', ['c', 'cpp'],
  version : '1.0.0',
  default_options : [
    'c_std=c99',
    'cpp_std=c++17',
    'optimization=2',
    'warning_level=2',
    'strip=true',
    'default_library=static',
  ],
)

gpu_scene = get_option('gpu_scene')
logging = get_option('logging')

if not logging
  add_project_arguments('-DNO_LOG', language : ['c', 'cpp'])
endif

subdir('src')
