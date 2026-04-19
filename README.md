# rufus-linux

Linux port of [Rufus](https://rufus.ie) — a utility for formatting and creating
bootable USB drives. Uses GTK4 for the UI and stays as close to the original
Windows layout as possible.

## Features

- Automatic detection of USB block devices (libudev)
- Direct write of isohybrid ISOs and raw disk images (`.img`)
- Image-type detection (isohybrid · ISO9660 · GPT · MBR via on-disk magic bytes)
- Partition table creation (MBR / GPT, libparted)
- Filesystem creation (FAT32 / exFAT / NTFS / ext4 / btrfs / UDF)
- Syslinux install for FreeDOS-style bootable FAT32 sticks
- Hash verification (MD5 / SHA-1 / SHA-256 / SHA-512, OpenSSL EVP)
- Background writer via GTask — UI stays responsive during writes

## Build

### Install dependencies (Debian / Ubuntu)

```bash
sudo apt install \
    meson ninja-build pkg-config \
    libgtk-4-dev \
    libudev-dev \
    libblkid-dev \
    libparted-dev \
    libssl-dev \
    libcurl4-openssl-dev
```

### Build

```bash
git clone https://github.com/yourname/rufus-linux
cd rufus-linux
meson setup build
meson compile -C build
```

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
└── src/
    ├── rufus.h         shared types and prototypes
    ├── main.c          GtkApplication entry point
    ├── ui.c            GTK4 main window
    ├── drive.c         USB drive enumeration (libudev)
    ├── iso.c           image-type detection (magic byte inspection)
    ├── part.c          partition table creation (libparted)
    ├── mkfs.c          fork+exec wrappers for mkfs.* and syslinux
    ├── format.c        write orchestrator
    ├── hash.c          MD5/SHA hashing (OpenSSL EVP)
    ├── worker.c        background worker (GTask)
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
| GUI | Win32 dialogs | GTK4 |
| Hashing | Win32 CryptAPI | OpenSSL EVP |
| Privilege elevation | UAC | polkit / pkexec |
| Settings storage | HKCU registry | `~/.config/rufus/settings.ini` (GKeyFile) |

## License

See [LICENSE.txt](LICENSE.txt). rufus-linux inherits GPLv3 from upstream Rufus.
