# 2ksbox: the 3dfx Voodoo 2 device (doc 21, M14). This directory is
# voodoo/ from the repository, overlaid into hw/voodoo by prepare-qemu.sh
# and reached by patch 62's subdir().
#
# 86Box's rasterizer (86box/, verbatim) is its own static library so it can
# be compiled against the shim of 86Box's headers (shim/) and with its own
# flags, and never sees QEMU's headers -- its "cpu.h" is shim/cpu.h, not
# target/i386's. The QEMU device and the shim's implementation are ordinary
# sourceset files. link_whole so the sourceset's static library, and the
# embed shared library built from its objects, carry every 86Box object.

voodoo86_src = files(
  '86box/vid_voodoo.c',
  '86box/vid_voodoo_blitter.c',
  '86box/vid_voodoo_display.c',
  '86box/vid_voodoo_fb.c',
  '86box/vid_voodoo_fifo.c',
  '86box/vid_voodoo_reg.c',
  '86box/vid_voodoo_render.c',
  '86box/vid_voodoo_setup.c',
  '86box/vid_voodoo_texture.c',
)

# 86Box is not written to QEMU's warning set; the code is upstream's
voodoo86_args = cc.get_supported_arguments(
  '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
  '-Wno-missing-prototypes', '-Wno-shadow', '-Wno-implicit-fallthrough',
  '-Wno-old-style-declaration', '-Wno-redundant-decls', '-Wno-sign-compare',
  '-Wno-strict-prototypes', '-Wno-type-limits', '-Wno-nested-externs',
  '-Wno-missing-format-attribute', '-Wno-undef', '-Wno-unused-function',
  '-Wno-stringop-overflow', '-Wno-maybe-uninitialized',
)
if host_machine.cpu_family() == 'x86_64'
  voodoo86_args += ['-msse2']
endif

voodoo86_lib = static_library('voodoo86', voodoo86_src,
                              include_directories: include_directories('.', 'shim'),
                              c_args: voodoo86_args,
                              pic: true,
                              build_by_default: false)
voodoo86_dep = declare_dependency(link_whole: voodoo86_lib)

i386_system_ss.add(files('voodoo2.c', 'voodoo_shim.c'), voodoo86_dep)
