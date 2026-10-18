project('dscn', 'cpp',
  version : '0.1.0',
  license : 'MIT',
  default_options : [
    'cpp_std=c++17',
    'warning_level=3',
    'buildtype=release',
  ],
)

libevdev_dep = dependency('libevdev')

executable('dscn', 'main.cpp',
  dependencies : [libevdev_dep],
  install : true,
  install_dir : get_option('bindir'),
)

install_data('misc/dscn.service', install_dir : 'lib/systemd/system')
install_data('misc/dscn.conf', install_dir : 'lib/modules-load.d')
