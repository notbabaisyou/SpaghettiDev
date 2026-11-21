dmabuf_sources = files('dmabuf.c')
dmabuf_dependencies = [common_dep]

if build_soft2d and host_machine.system() == 'linux' and conf_data.get('BUSFAULT').to_int() != 0
    dmabuf_sources += files('dmabuf-fb.c')
    dmabuf_dependencies += libdrm_dep
endif

libxserver_dmabuf = static_library('libxserver_dmabuf',
    dmabuf_sources,
    include_directories: inc,
    dependencies: dmabuf_dependencies,
)

if build_xorg
    install_data('dmabuf.h', install_dir: xorgsdkdir)
endif
