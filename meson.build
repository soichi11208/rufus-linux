project('rufus-linux', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'b_pie=true',
  ],
)

cc = meson.get_compiler('c')
i18n = import('i18n')

gtk_dep    = dependency('gtk4',      version: '>=4.10')
udev_dep   = dependency('libudev')
blkid_dep  = dependency('blkid')
crypto_dep = dependency('libcrypto')
curl_dep   = dependency('libcurl')
thread_dep = dependency('threads')
parted_dep = dependency('libparted')

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

common_args = [
  '-D_GNU_SOURCE',
  '-DRUFUS_VERSION="@0@"'.format(meson.project_version()),
  '-DRUFUS_LOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
]
if have_nls
  common_args += '-DENABLE_NLS=1'
endif

sources = files(
  'src/main.c',
  'src/ui.c',
  'src/drive.c',
  'src/format.c',
  'src/hash.c',
  'src/iso.c',
  'src/log.c',
  'src/mkfs.c',
  'src/part.c',
  'src/privops.c',
  'src/util.c',
  'src/worker.c',
  'src/badblocks.c',
  'src/settings.c',
  'src/wue.c',
  'src/i18n.c',
)

deps = [gtk_dep, udev_dep, blkid_dep, crypto_dep, curl_dep, thread_dep, parted_dep]

if have_cdio
  sources += files('src/iso_extract.c')
  deps    += [cdio_dep, iso_dep]
  common_args += '-DHAVE_LIBCDIO=1'
else
  # Compile a stub so format.c can still link when libcdio is missing.
  sources += files('src/iso_extract_stub.c')
endif

if have_nls and intl_dep.found()
  deps += intl_dep
endif

executable('rufus-linux',
  sources,
  c_args: common_args,
  dependencies: deps,
  install: true,
)

if have_nls
  subdir('po')
endif

install_data('res/org.rufus.linux.policy',
  install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)
