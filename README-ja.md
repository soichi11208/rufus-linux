# rufus-linux

[Rufus](https://rufus.ie) の Linux 移植版です。USB フラッシュドライブのフォーマットとブータブルドライブの作成を行います。GUI は GTK4 を使用しており、Windows 版のレイアウトをできる限り再現しています。

## 機能

- USB ブロックデバイスの自動検出（libudev）
- isohybrid ISO / 生ディスクイメージ（.img）の直接書き込み
- 起動種別の自動判定（isohybrid・ISO9660・GPT・MBR を on-disk のマジックバイトで識別）
- パーティションテーブルの作成（MBR / GPT、libparted）
- ファイルシステムの作成（FAT32 / exFAT / NTFS / ext4 / btrfs / UDF）
- Syslinux インストール（FreeDOS 形式の FAT32 ブータブルスティック）
- ハッシュ計算（MD5 / SHA-1 / SHA-256 / SHA-512、OpenSSL EVP）
- バックグラウンドワーカーによる書き込み（GTask）— 書き込み中も UI は応答可能

## ビルド方法

### 依存パッケージのインストール（Debian / Ubuntu）

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

### ビルド

```bash
git clone https://github.com/soichi11208/rufus-linux
cd rufus-linux
meson setup build
meson compile -C build
```

### インストール（polkit 連携を使う場合）

```bash
sudo meson install -C build
```

polkit アクション定義（`res/org.rufus.linux.policy`）が `/usr/share/polkit-1/actions/` にインストールされます。

## 実行方法

USB ブロックデバイスへの書き込みには root 権限が必要です。

```bash
# 開発中の手軽な方法
sudo ./build/rufus-linux

# インストール後は pkexec 経由でGUI認証ダイアログが表示される
pkexec /usr/bin/rufus-linux
```

## 使い方

1. **デバイス** — `↺` ボタンで USB ドライブを検索し、ドロップダウンから対象を選択
2. **Boot selection** — "Disk or ISO image" を選んで **SELECT** ボタンでイメージファイルを指定
3. **Image option** — 通常は変更不要（isohybrid ISO は自動検出）
4. **Partition scheme / Target system** — デフォルト（MBR / BIOS or UEFI）でほとんどの用途に対応
5. **Format Options** — ボリュームラベル・ファイルシステム・クラスタサイズを必要に応じて変更
6. **START** — 確認ダイアログの後、バックグラウンドで書き込みを開始

## 対応フォーマット

| ファイルシステム | コマンド | 備考 |
|---|---|---|
| FAT32 | `mkfs.fat` | dosfstools パッケージ |
| exFAT | `mkfs.exfat` | exfatprogs パッケージ |
| NTFS | `mkfs.ntfs` | ntfs-3g パッケージ |
| ext4 | `mkfs.ext4` | e2fsprogs パッケージ |
| btrfs | `mkfs.btrfs` | btrfs-progs パッケージ |
| UDF | `mkudffs` | udftools パッケージ |

## ファイル構成

```
rufus-linux/
├── meson.build
├── README.md           (英語)
├── README-ja.md        (このファイル)
├── res/
│   └── org.rufus.linux.policy   # polkit アクション定義
└── src/
    ├── rufus.h         共通型・プロトタイプ宣言
    ├── main.c          GtkApplication エントリーポイント
    ├── ui.c            GTK4 メインウィンドウ
    ├── drive.c         USB ドライブ列挙（libudev）
    ├── iso.c           イメージ種別判定（マジックバイト検査）
    ├── part.c          パーティションテーブル作成（libparted）
    ├── mkfs.c          mkfs.* / syslinux の fork+exec ラッパー
    ├── format.c        書き込みオーケストレータ
    ├── hash.c          MD5/SHA ハッシュ計算（OpenSSL EVP）
    ├── worker.c        バックグラウンドワーカー（GTask）
    ├── privops.c       pkexec 権限昇格
    └── log.c           ログ出力（stderr + アプリ内バッファ）
```

## Windows 版との主な違い

| 機能 | Windows 版 | この Linux 版 |
|---|---|---|
| デバイス列挙 | SetupAPI + VDS | libudev |
| ディスク操作 | DeviceIoControl | ioctl (BLKGETSIZE64 等) |
| パーティション | VDS | libparted |
| ファイルシステム | 内蔵フォーマッタ | mkfs.* fork+exec |
| GUI | Win32 ダイアログ | GTK4 |
| ハッシュ | Win32 CryptAPI | OpenSSL EVP |
| 権限昇格 | UAC | polkit / pkexec |
| 設定の保存 | HKCU レジストリ | `~/.config/rufus/settings.ini` (GKeyFile) |

## ライセンス

[LICENSE.txt](LICENSE.txt) を参照してください。上流 Rufus の GPLv3 を継承しています。

