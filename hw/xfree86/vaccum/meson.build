vaccum_mod_src = [
    '../../../vaccum/spirv_builder.c',
    '../../../vaccum/vaccum.c',
    '../../../vaccum/vaccum_core.c',
    '../../../vaccum/vaccum_copy.c',
    '../../../vaccum/vaccum_dri3.c',
    '../../../vaccum/vaccum_dri3_syncobj.c',
    '../../../vaccum/vaccum_glyphbit.c',
    '../../../vaccum/vaccum_image.c',
    '../../../vaccum/vaccum_lines.c',
    '../../../vaccum/vaccum_prepare.c',
    '../../../vaccum/vaccum_points.c',
    '../../../vaccum/vaccum_rects.c',
    '../../../vaccum/vaccum_spans.c',
    '../../../vaccum/vaccum_segs.c',
    '../../../vaccum/vaccum_text.c',
    '../../../vaccum/vaccum_transfer.c',
    '../../../vaccum/vaccum_vulkan.c',
    '../../../vaccum/vaccum_ximage.c',
    '../../../vaccum/vaccum_module.c',
]

shared_module(
    'vaccum',
    vaccum_mod_src,

    include_directories: [inc, xorg_inc],
    c_args: xorg_c_args,
    dependencies: [
        common_dep,
        vulkan_dep,
        libdrm_dep,
        gbm_dep,
    ],
    install: true,
    install_dir: module_dir,
)
