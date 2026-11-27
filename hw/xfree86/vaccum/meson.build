vaccum_lib = shared_module(
    'vaccum',
    '../../../vaccum/vaccum_gbm.c',
    '../../../vaccum/vaccum_screen.c',
    '../../../vaccum/vaccum_module.c',
    include_directories: [inc, xorg_inc],
    c_args: xorg_c_args,
    dependencies: [common_dep, libdrm_dep, gbm_dep, vulkan_dep],
    link_with: vaccum,
    install: true,
    install_dir: module_dir,
)
