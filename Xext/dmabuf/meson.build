libxserver_dmabuf = static_library('libxserver_dmabuf',
    'dmabuf.c',
    include_directories: inc,
    dependencies: [common_dep],
)

if build_xorg
    install_data('dmabuf.h', install_dir: xorgsdkdir)
endif
