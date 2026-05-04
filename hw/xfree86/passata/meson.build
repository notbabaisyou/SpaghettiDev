passata_sources = [
    'passata.c',
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
    ],
    link_with: [ libxserver_exa ],
    install: true,
    install_dir: module_dir,
)