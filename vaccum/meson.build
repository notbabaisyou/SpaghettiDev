srcs_vaccum = [
    'spirv_builder.c',
    'spirv_builder.h',
    'vaccum.c',
    'vaccum_core.c',
    'vaccum_dri3.c',
    'vaccum_dri3_syncobj.c',
    'vaccum_glyphbit.c',
    'vaccum_image.c',
    'vaccum_lines.c',
    'vaccum_linear.c',
    'vaccum_prepare.c',
    'vaccum_prepare.h',
    'vaccum_points.c',
    'vaccum_rects.c',
    'vaccum_spans.c',
    'vaccum_segs.c',
    'vaccum_text.c',
    'vaccum_transfer.c',
    'vaccum_transfer.h',
    'vaccum_vulkan.c',
    'vaccum_ximage.c',
]

vaccum_pci_deps = []
vaccum_c_args = []
if get_option('pciaccess') and pciaccess_dep.found()
    vaccum_pci_deps += pciaccess_dep
    vaccum_c_args += '-DVACCUM_HAVE_PCIACCESS'
endif

vaccum = static_library('vaccum',
    srcs_vaccum,
    include_directories: inc,
    c_args: vaccum_c_args,
    dependencies: vaccum_pci_deps + [
        common_dep,
        vulkan_dep,
        libdrm_dep,
    ],
)
