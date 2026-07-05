# rufus-linux

Linux port of [Rufus](https://rufus.ie) — a utility for formatting and creating
bootable USB drives. The UI is built on [stilus](https://github.com/soichi11208/stilus),
a minimal C++20 GUI toolkit that fully statically links (no runtime GUI
dependencies), while staying as close to the original Windows layout as
possible.

## Features

- Automatic detection of USB block devices (libudev)
- Direct write of isohybrid ISOs and raw disk images (`.img`)
- Image-type detection (isohybrid · ISO9660 · GPT · MBR via on-disk magic bytes)
- Partition table creation (MBR / GPT, libparted)
- Filesystem creation (FAT32 / exFAT / NTFS / ext4 / btrfs / UDF)
- Syslinux install for FreeDOS-style bootable FAT32 sticks
- Hash verification (MD5 / SHA-1 / SHA-256 / SHA-512, OpenSSL EVP)
- Background writer thread — UI stays responsive during writes
- Self-contained GUI via stilus (Wayland / X11 backends, no GTK dependency)

## Build

### Install dependencies (Debian / Ubuntu)

```bash
sudo apt install \
    meson ninja-build pkg-config \
    g++ \
    libudev-dev \
    libblkid-dev \
    libparted-dev \
    libssl-dev \
    libcurl4-openssl-dev
```

stilus is bundled as a git submodule (`third_party/stilus`) — no external GUI
toolkit needs to be installed.

### Build

```bash
git clone --recurse-submodules https://github.com/soichi11208/rufus-linux
cd rufus-linux
meson setup build
meson compile -C build
```

If you already cloned without submodules, run
`git submodule update --init --recursive` before building.

### Install (for polkit integration)

```bash
sudo meson install -C build
```

The polkit action definition (`res/org.rufus.linux.policy`) is installed to
`/usr/share/polkit-1/actions/`.

## Running

Writing to a block device requires root privileges.

```bash
# Quick path for development
sudo ./build/rufus-linux

# After install — a GUI auth dialog appears via pkexec
pkexec /usr/bin/rufus-linux
```

## How to use

1. **Device** — click the `↺` button to scan for USB drives, then select the target from the dropdown
2. **Boot selection** — choose "Disk or ISO image" and click **SELECT** to pick an image file
3. **Image option** — usually no change needed (isohybrid ISOs are auto-detected)
4. **Partition scheme / Target system** — the defaults (MBR / BIOS or UEFI) cover most use cases
5. **Format Options** — adjust volume label, filesystem, and cluster size if needed
6. **START** — after the confirmation dialog, writing begins in the background

## Supported filesystems

| Filesystem | Command | Package |
|---|---|---|
| FAT32 | `mkfs.fat` | dosfstools |
| exFAT | `mkfs.exfat` | exfatprogs |
| NTFS | `mkfs.ntfs` | ntfs-3g |
| ext4 | `mkfs.ext4` | e2fsprogs |
| btrfs | `mkfs.btrfs` | btrfs-progs |
| UDF | `mkudffs` | udftools |

## Layout

```
rufus-linux/
├── meson.build
├── README.md           (English)
├── README-ja.md        (Japanese)
├── LICENSE.txt
├── res/
│   └── org.rufus.linux.policy   # polkit action definition
├── third_party/
│   └── stilus/        stilus GUI toolkit (git submodule)
└── src/
    ├── rufus.h         shared types and prototypes
    ├── main.cpp        stilus application entry point
    ├── ui.cpp          stilus main window
    ├── drive.c         USB drive enumeration (libudev)
    ├── iso.c           image-type detection (magic byte inspection)
    ├── part.c          partition table creation (libparted)
    ├── mkfs.c          fork+exec wrappers for mkfs.* and syslinux
    ├── format.c        write orchestrator
    ├── hash.c          MD5/SHA hashing (OpenSSL EVP)
    ├── worker.cpp      background worker thread
    ├── privops.c       pkexec privilege elevation
    └── log.c           logging (stderr + in-app buffer)
```

## Differences from the Windows build

| Feature | Windows | This Linux port |
|---|---|---|
| Device enumeration | SetupAPI + VDS | libudev |
| Disk I/O | DeviceIoControl | ioctl (BLKGETSIZE64 etc.) |
| Partitioning | VDS | libparted |
| Filesystem | built-in formatters | mkfs.* fork+exec |
| GUI | Win32 dialogs | stilus (C++20, Wayland/X11, statically linked) |
| Hashing | Win32 CryptAPI | OpenSSL EVP |
| Privilege elevation | UAC | polkit / pkexec |
| Settings storage | HKCU registry | `~/.config/rufus/settings.ini` (GKeyFile) |

## License

See [LICENSE](LICENSE). rufus-linux inherits GPLv3 from upstream Rufus.
