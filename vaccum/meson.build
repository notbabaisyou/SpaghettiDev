srcs_vaccum = [
    'spirv_builder.c',
    'spirv_builder.h',
    'vaccum.c',
    'vaccum_core.c',
    'vaccum_glyphbit.c',
    'vaccum_image.c',
    'vaccum_lines.c',
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

vulkan_dep = dependency('vulkan')

vaccum = static_library('vaccum',
    srcs_vaccum,
    include_directories: inc,
    dependencies: [
        common_dep,
	vulkan_dep,
    ],
)
