project('rufus-linux', 'c', 'cpp',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=2',
    'b_pie=true',
  ],
)

cc  = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
i18n = import('i18n')
fs   = import('fs')

udev_dep   = dependency('libudev')
blkid_dep  = dependency('blkid')
crypto_dep = dependency('libcrypto', static: true)
curl_dep   = dependency('libcurl', static: true)
thread_dep = dependency('threads')
parted_dep = dependency('libparted', static: true)
uuid_dep   = cxx.find_library('uuid', required: false)
dm_dep      = cxx.find_library('devmapper', required: false)

# libcdio is split across several .pc files in Debian/Fedora; the ISO9660
# bits are needed for non-isohybrid extraction. If unavailable, the build
# is still functional for isohybrid + raw image writing only.
cdio_dep   = dependency('libcdio',         required: false)
iso_dep    = dependency('libcdio_iso9660', required: false)
have_cdio  = cdio_dep.found() and iso_dep.found()

# gettext: optional. Without it, _() reduces to identity.
intl_dep   = cc.find_library('intl', required: false)
have_nls   = (find_program('msgfmt', required: false).found() and
              find_program('xgettext', required: false).found())

# ---------------------------------------------------------------------------
# stilus (the GUI toolkit; https://github.com/soichi11208/stilus-GUI).
# The stilus checkout may live anywhere — override with
# `-Dstilus_root=/path/to/stilus` at meson-setup time. The default assumes a
# sibling checkout, and we fall back to a couple of known development
# locations so a fresh clone Just Works on this machine.
# ---------------------------------------------------------------------------
stilus_root = get_option('stilus_root')
if stilus_root == ''
    candidates = [
      meson.current_source_dir() / 'third_party' / 'stilus',
      meson.current_source_dir() / '..' / 'stilus',
      meson.current_source_dir() / '..',
      '/mnt/ssd4/stilus',
    ]
  foreach cand : candidates
    if stilus_root == '' and fs.is_dir(cand / 'include' / 'stilus')
      stilus_root = cand
    endif
  endforeach
endif
if stilus_root == ''
  error('stilus checkout not found — set with -Dstilus_root=/path/to/stilus')
endif

stilus_include = stilus_root / 'include'
stilus_lib     = stilus_root / 'build' / 'libstilus.a'
stilus_xcb_dir = stilus_root / 'third_party' / 'xcb' / 'lib'

if not fs.exists(stilus_lib)
  error('stilus not built — run `meson compile -C build` in @0@'.format(stilus_root))
endif

stilus_link_args = [
  stilus_lib,
  stilus_xcb_dir / 'libxcb.a',
  stilus_xcb_dir / 'libxcb-render.a',
  stilus_xcb_dir / 'libxcb-shm.a',
  stilus_xcb_dir / 'libxcb-xfixes.a',
  stilus_xcb_dir / 'libxcb-xkb.a',
]
add_project_link_arguments(stilus_link_args, language: 'cpp')

xau_dep   = cxx.find_library('Xau', static: true)
xdmcp_dep = cxx.find_library('Xdmcp', static: true)

common_args = [
  '-D_GNU_SOURCE',
  '-DRUFUS_VERSION="@0@"'.format(meson.project_version()),
  '-DRUFUS_LOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
]
if have_nls
  common_args += '-DENABLE_NLS=1'
endif

# C++ sources additionally need stilus's public headers.
cpp_args = common_args + ['-I' + stilus_include]

sources_c = files(
  'src/drive.c',
  'src/format.c',
  'src/hash.c',
  'src/iso.c',
  'src/log.c',
  'src/mkfs.c',
  'src/part.c',
  'src/privops.c',
  'src/util.c',
  'src/badblocks.c',
  'src/settings.c',
  'src/wue.c',
  'src/i18n.c',
)

sources_cpp = files(
  'src/main.cpp',
  'src/ui.cpp',
  'src/worker.cpp',
)

deps = [udev_dep, blkid_dep, crypto_dep, curl_dep, thread_dep, parted_dep,
        xau_dep, xdmcp_dep, uuid_dep, dm_dep]

if have_cdio
  sources_c += files('src/iso_extract.c')
  deps      += [cdio_dep, iso_dep]
  common_args += '-DHAVE_LIBCDIO=1'
else
  # Compile a stub so format.c can still link when libcdio is missing.
  sources_c += files('src/iso_extract_stub.c')
endif

if have_nls and intl_dep.found()
  deps += intl_dep
endif

executable('rufus-linux',
  sources_c + sources_cpp,
  c_args: common_args,
  cpp_args: cpp_args,
  dependencies: deps,
  install: true,
)

if have_nls
  subdir('po')
endif

install_data('res/org.rufus.linux.policy',
  install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)
