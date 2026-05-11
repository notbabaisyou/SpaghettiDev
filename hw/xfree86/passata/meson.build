passata_sources = [
    'passata_egl.c',
    'passata_pixmap.c',
    'passata.c'
]

passata_inc = [
    include_directories('.')
]

shared_module('passata',
    passata_sources,
    include_directories: [inc, xorg_inc, passata_inc],
    c_args: xorg_c_args,
    dependencies: [
        common_dep,
        epoxy_dep,
        dependency('libdrm', version: '>= 2.4.46'),
    ],
    link_with: [ libxserver_exa ],
    install: true,
    install_dir: module_dir,
)