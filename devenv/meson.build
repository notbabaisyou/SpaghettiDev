if not build_xorg
  subdir_done()
endif

xorg_path = meson.project_build_root() / 'hw' / 'xfree86' / 'Xorg'

startx_conf = configuration_data()
startx_conf.set('XORG', xorg_path)

# Use configure_file to substitute @XORG@ then use a custom_target to set
# the executable bit. configure_file alone does not make the output executable
# when install is false.
startx_configured = configure_file(
  input: 'startx.in',
  output: 'startx.configured',
  configuration: startx_conf,
)

startx = custom_target('startx',
  input: startx_configured,
  output: 'startx',
  command: ['sh', '-c', 'cp "@INPUT@" "@OUTPUT@" && chmod +x "@OUTPUT@"'],
  install: false,
  build_by_default: true,
)

meson.add_devenv({'PATH': meson.current_build_dir()}, method: 'prepend')
