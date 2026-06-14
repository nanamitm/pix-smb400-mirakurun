# USB ブートイメージ ビルドガイド

PIX-SMB400 の USB Boot 用ファイルの仕組みとビルド手順です。

USB メモリには以下の 3 ファイルが必要です。いずれも `boot/` のスクリプトで生成します。

| ファイル | 役割 | 生成方法 |
|----------|------|----------|
| `bootargs.bin` | u-boot 環境変数ブロック（`androidboot.selinux=permissive` を注入） | `make_usb_boot.py` |
| `root_rsa_pub_crc.bin` | 外部 RSA 公開鍵。BootROM がこの鍵で `bootargs.bin` を検証する | `make_usb_boot.py` |
| `initramfs_patched.uimg` | カスタム initramfs（ADB・DHCP・Mirakurun 自動起動を組み込み） | `build_initramfs.sh` |

---

## クイックスタート

### 1. bootargs.bin / root_rsa_pub_crc.bin を生成する

要件: Docker（または Python 3 + `pycryptodome`）

```sh
cd boot

docker run --rm \
  -v "$(pwd):/usb_boot" \
  python:3.11-slim bash -c "
pip install pycryptodome -q
cd /usb_boot && python3 make_usb_boot.py
"
cd ..
```

→ `boot/bootargs.bin` と `boot/root_rsa_pub_crc.bin`（および `rsa_key.pem`）が生成されます。

### 2. initramfs_patched.uimg をビルドする

要件: Docker と、用意した `kernel.img`

```sh
# kernel.img から initramfs を取り出す
binwalk -e kernel.img
# → 展開された大きい cpio ファイルが initramfs（例: .../_kernel.img.extracted/988000）

# ビルド
bash boot/build_initramfs.sh /path/to/_kernel.img.extracted/988000
```

→ `boot/initramfs_patched.uimg` が生成されます。

### 3. USB メモリにコピーする

```sh
# FAT32 でフォーマット（PIXBOOT というラベルを付ける）
sudo mkfs.fat -F 32 -n PIXBOOT /dev/sdX1

# マウント（自動マウントされない場合）
sudo mkdir -p /mnt/PIXBOOT
sudo mount -o uid=$(id -u),gid=$(id -g) /dev/sdX1 /mnt/PIXBOOT
sudo chown $USER:$USER /mnt/PIXBOOT

# コピー
cp boot/bootargs.bin           /mnt/PIXBOOT/
cp boot/root_rsa_pub_crc.bin   /mnt/PIXBOOT/
cp boot/initramfs_patched.uimg /mnt/PIXBOOT/

# アンマウントして取り出す
sudo umount /mnt/PIXBOOT
```

USB メモリを PIX-SMB400 に挿入し、USB Boot ピンをショートして電源を入れます。
起動・ADB 接続の手順は [README.md](README.md) を参照してください。

---

# 技術ドキュメント

## 1. USB Boot の仕組み

Hi3798CV200 (PIX-SMB400 の SoC) の BootROM には **USB Boot モード**が搭載されています。

基板上の特定ピンを電源投入時にショートすると、BootROM は内部 eMMC の代わりに
USB メモリ上のファイルを読み込みます。

```
通常起動:  eMMC (mmcblk0p2) の公開鍵で bootargs を検証 → Android TV として動作
USB Boot:  USB メモリ上の root_rsa_pub_crc.bin を公開鍵として採用 → カスタム initramfs を起動
```

このとき eMMC のパーティションは変更されないため、USB メモリを抜いて再起動すれば
元の Android TV に戻ります。

### RSA 鍵差し替えの原理

BootROM は `root_rsa_pub_crc.bin` が USB メモリに存在すると、eMMC の内部鍵を使わず
**外部鍵で検証**を行います。そのため Hisilicon の秘密鍵は不要で、
自前で生成した鍵ペアで署名・検証できます。

> **注意:** BootROM の RSA 指数は `e=3`。外部鍵も `e=3` で生成する必要があります。
> `make_usb_boot.py` はこれを自動的に設定します。

### init バイナリへのパッチ

この SoC の `init` (Android のプロセス 1) は、カーネルパラメータで
`androidboot.selinux=permissive` を渡しても起動直後に SELinux を強制 enforcing に戻します。

`patch_init.py` は 2 バイトのバイナリパッチでこれを無効化します:

| オフセット | 変更前 | 変更後 | 効果 |
|------------|--------|--------|------|
| `0xcd1d` | `0xd0` (BEQ) | `0xe0` (B) | `security_setenforce(1)` を永遠にスキップ |
| `0x14160` | `0x2f` (`/`) | `0x5f` (`_`) | `/system/etc/prop.default` のパスを壊し `ro.debuggable=0` の先読みを防ぐ |

---

## 2. boot/ ディレクトリの構成

```
boot/
├── make_usb_boot.py                bootargs.bin / root_rsa_pub_crc.bin 生成スクリプト
├── patch_init.py                   init バイナリパッチスクリプト
├── build_initramfs.sh              initramfs_patched.uimg ビルドスクリプト
└── initramfs_overlay/              initramfs に追加・置換されるファイル
    ├── default.prop                ro.debuggable=1 / ro.adb.secure=0
    ├── dhclient.conf               DHCP クライアント設定
    ├── init.pixboot.rc             PIX-SMB400 専用 init サービス定義
    ├── init_pix_netdbg.sh          Ethernet DHCP セットアップ（PHY 初期化待ち含む）
    ├── start_proxy.sh              デバイス起動時に /data/local/tmp/ へ自動デプロイ
    ├── crash_guard.sh              同上
    ├── stop_android_tv.sh          同上
    ├── smb400_tuner.sh             同上
    └── initrc/
        └── logd.rc                 logd を無効化（このカーネルでは capset() で EPERM）
```

ビルド時に生成されるファイル（リポジトリには含まれません）:

| ファイル | サイズ | 内容 |
|----------|--------|------|
| `bootargs.bin` | 65,536 bytes | u-boot env ブロック（CRC32 検証済み） |
| `root_rsa_pub_crc.bin` | 264 bytes | RSA モジュラス (256 B) + 指数 (4 B) + CRC32 (4 B) |
| `rsa_key.pem` | — | RSA 秘密鍵（e=3）。紛失すると再生成が必要。漏洩注意 |
| `initramfs_patched.uimg` | — | カスタム initramfs |

---

## 3. bootargs.bin / root_rsa_pub_crc.bin の生成

`boot/rsa_key.pem` が存在すれば既存の鍵を再利用します（USB Boot ピン認証に影響なし）。
`rsa_key.pem` がなければ新規に鍵ペアを生成します。

**要件:** Docker または Python 3 + `pycryptodome`

### Docker を使う場合（推奨）

```sh
cd boot

docker run --rm \
  -v "$(pwd):/usb_boot" \
  python:3.11-slim bash -c "
pip install pycryptodome -q
cd /usb_boot && python3 make_usb_boot.py
"
```

### Python を直接使う場合

```sh
pip install pycryptodome
cd boot
python3 make_usb_boot.py
```

生成されるファイル:

| ファイル | サイズ | 内容 |
|----------|--------|------|
| `bootargs.bin` | 65,536 bytes | u-boot env ブロック（CRC32 検証済み） |
| `root_rsa_pub_crc.bin` | 264 bytes | RSA モジュラス (256 B) + 指数 (4 B) + CRC32 (4 B) |
| `rsa_key.pem` | — | RSA 秘密鍵（e=3）。紛失すると再生成が必要 |

> `rsa_key.pem` を変更した場合は USB メモリ上の `root_rsa_pub_crc.bin` も更新してください。

---

## 4. initramfs_patched.uimg のビルド

ファームウェアの initramfs に `init` バイナリのパッチと init スクリプトを
組み込んだカスタム initramfs を生成します。

### 前提

- **Docker** が使用可能なこと
- **kernel.img** が手元にあること

用意した `kernel.img` から initramfs cpio を取り出します:

```sh
# binwalk でカーネルイメージから initramfs を取り出す
binwalk -e kernel.img

# 展開されたファイルのうち大きい cpio ファイルが initramfs
# 例: _kernel.img.extracted/988000
```

### ビルド実行

リポジトリのルートで実行します。

```sh
bash boot/build_initramfs.sh /path/to/_kernel.img.extracted/988000
```

完了すると `boot/initramfs_patched.uimg` が生成されます。

```sh
# 確認
ls -lh boot/initramfs_patched.uimg
```

### オーバーレイのカスタマイズ

`boot/initramfs_overlay/` 内のファイルを編集してからビルドすると、
変更が initramfs に反映されます。

**主な編集対象:**

| ファイル | 用途 |
|----------|------|
| `init.pixboot.rc` | 起動時サービス定義。自動起動タイミングや追加コマンドを変更 |
| `start_proxy.sh` | Mirakurun 自動起動スクリプト（電源 ON 時に実行される版） |
| `smb400_tuner.sh` | チューナー制御スクリプト |
| `default.prop` | デバッグプロパティ（変更不要） |

これらの `.sh` は起動時に `init.pixboot.rc` によって `/data/local/tmp/` へ自動コピーされます。
手動デプロイが不要になるため、スクリプトを更新したら initramfs を再ビルドして USB メモリを更新してください。

---

## 5. USB メモリの更新

ビルド後は USB メモリを更新します。

```sh
# マウント（自動マウントされない場合）
sudo mkdir -p /mnt/PIXBOOT
sudo mount -o uid=$(id -u),gid=$(id -g) /dev/sdX1 /mnt/PIXBOOT
sudo chown $USER:$USER /mnt/PIXBOOT

# コピー
cp boot/bootargs.bin           /mnt/PIXBOOT/
cp boot/root_rsa_pub_crc.bin   /mnt/PIXBOOT/
cp boot/initramfs_patched.uimg /mnt/PIXBOOT/

# アンマウントして取り出す
sudo umount /mnt/PIXBOOT
```

USB メモリを PIX-SMB400 に挿入し、USB Boot ピンをショートして電源を入れます。

---

## トラブルシューティング

| 症状 | 原因 | 対処 |
|------|------|------|
| `get extern rsa key` がシリアルに出ない | USB Boot ピンのショートタイミング | 電源投入**前**にショートを維持 |
| `CRC32 check failed` | `bootargs.bin` が破損 | Section 3 で再生成 |
| ADB で `uid=2000(shell)` | eMMC 通常ブートしている | USB ブートを再確認 |
| SELinux が enforcing のまま | init パッチ未適用 | `patch_init.py` が正常終了したか確認 |
| デバイスの IP アドレスが不明 | DHCP 未取得 | ブート後 30 秒以上待つ / ルーター側で確認 |
| `[!] unexpected byte` (patch_init.py) | 別バージョンの `init` バイナリ | オフセットの再特定が必要（ARM Thumb2 逆アセンブル） |
