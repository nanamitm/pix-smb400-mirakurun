# PIX-SMB400 Mirakurun

PIX-SMB400（HiSilicon Hi3798CV200 搭載 Android TV）上で Mirakurun を実行し、地上波（ISDB-T）・BS（ISDB-S）・110度CS（ISDB-S）・BS4K / BS8K（ISDB-S3）を受信するためのプロジェクトです。

<img width="1052" height="822" alt="image" src="https://github.com/user-attachments/assets/fd564f7a-a7b6-4b5c-941d-a226162a170c" />

## 免責事項

本プロジェクトのファイル・スクリプト・ドキュメントは、技術的な調査・学習を目的としています。
利用はすべて**自己責任**で行ってください。

- 本手順はメーカー非公式・非サポートの改造であり、実行した時点でメーカー保証は失われます。
- ブートローダや init への介入を伴うため、**デバイスが文鎮化（起動不能）する可能性**があります。
  実際に過去、特定の操作でデバイスが起動不能になった事例があります。
- 作者は、本プロジェクトの利用によって生じた**いかなる損害（デバイスの故障・データ消失・経済的損失・第三者への損害等を含むがこれに限らない）についても、  一切の責任を負いません。**
- 放送の受信・録画・復号は、利用者自身が正規に契約・受信権を持つ範囲で、私的利用の目的に限り行ってください。
  関係する法令・契約・利用規約は各自の責任で遵守してください。

---

## 全体の流れ

```
Part 1: USB ブートで root を取る
  Step 1  ブートファイルをビルドして USB メモリを準備する
  Step 2  PIX-SMB400 を USB ブートで起動する
  Step 3  ADB で接続確認する

Part 2: Mirakurun のセットアップ
  Step 4  Alpine Linux + Node.js をセットアップする
  Step 5  Mirakurun をデプロイする
  Step 6  バイナリをビルドしてデプロイする
  Step 7  ACAS マスターキーを設定する

Part 3: 起動・確認
  Step 8  Mirakurun を起動する
  Step 9  BS4K ストリームを確認する
```

---

## 必要なもの

| 項目 | 内容 |
|------|------|
| PIX-SMB400 本体 | USB ブートピンにアクセスできる状態 |
| USB メモリ | FAT32 フォーマット、1 GB 以上 |
| ビルド環境 | Docker（ブートファイル用）、`gcc-arm-linux-gnueabi` + `libssl-dev`（バイナリのビルド用） |
| ACAS マスターキー | 64 文字の hex |

---

## 開発環境（Dev Container）

本リポジトリには [Dev Container](https://containers.dev/) 設定（[.devcontainer/](.devcontainer/)）が含まれています。
ビルドに必要なツールチェーンを一括で用意できるため、**ローカルへ個別にパッケージをインストールせずに済みます**。

VS Code の **Dev Containers** 拡張、または GitHub Codespaces で「Reopen in Container」すると、
[.devcontainer/Dockerfile](.devcontainer/Dockerfile) からビルド環境が構築されます。主な内容:

- `gcc-arm-linux-gnueabi` + `libssl-dev` — `make build-bins`（ARM32 バイナリのクロスコンパイル）
- `binwalk` + `cpio` — `kernel.img` から initramfs cpio を展開（[Step 0](#step-0-kernelimg-を入手して展開する)）
- `adb` — デバイスとのバイナリ転送・android-libs 取得
- `python3-pycryptodome` — `make_usb_boot.py`（`bootargs.bin` / RSA 鍵生成）
- `nodejs` / `npm` / `git` / `curl` — Mirakurun-BS4K のクローン・ビルド・デプロイ
- **docker-in-docker** feature — `build_initramfs.sh` / `make_usb_boot.py` がコンテナ内で `docker run` を使うため有効化済み

> 以降の手順に出てくる `sudo apt install ...`（`gcc-arm-linux-gnueabi`・`libssl-dev`・`binwalk` 等）は、
> Dev Container を使う場合はインストール済みのためスキップできます。
> ただし USB メモリのフォーマット・マウント（[Step 1](#step-1-ブートファイルをビルドして-usb-メモリを準備する)）や
> デバイスへの物理アクセスを伴う操作は、ホスト側で実施してください。

---

## ディレクトリ構成

```
.                                    （リポジトリのルート）
├── README.md                        このファイル
├── BOOT.md                          USB ブートイメージの仕組みと再ビルド手順
├── Makefile                         デプロイ・運用コマンド集
├── .devcontainer/                   Dev Container 設定（ビルド環境一式）
│   ├── devcontainer.json            Dev Container 定義（docker-in-docker feature 等）
│   └── Dockerfile                   ビルド依存パッケージのインストール
├── boot/                            USB ブート用ファイル（ビルド手順は BOOT.md）
│   ├── make_usb_boot.py             bootargs.bin / root_rsa_pub_crc.bin 生成スクリプト
│   ├── patch_init.py                init バイナリパッチスクリプト（SELinux bypass 等）
│   ├── build_initramfs.sh           initramfs_patched.uimg ビルドスクリプト
│   └── initramfs_overlay/           initramfs オーバーレイファイル
├── bin/                             ビルドしたバイナリの出力先（make build-bins で生成）
├── include/openssl/                 b61dec ビルド用 OpenSSL 設定ヘッダ
├── patches/                         デプロイ時に適用するパッチ（@node-rs/crc32 の JS シム）
├── scripts/
│   ├── smb400-tuner.sh              Mirakurun チューナーコマンドラッパー
│   ├── start_mirakurun.sh           Mirakurun 起動スクリプト（手動実行用）
│   ├── stop_android_tv.sh           Android TV 不要プロセス停止
│   ├── crash_guard.sh               クラッシュ監視ウォッチドッグ
│   └── setup_proot.sh               Alpine + Node.js 初回セットアップ
├── config/
│   ├── tuners.yml                   Mirakurun チューナー設定
│   ├── channels.yml                 GR / BS / CS / BS4K / BS8K チャンネル一覧
│   ├── bs_tsid.conf                 BS チャンネル → 実 MPEG TS-ID 表（scan_bs_tsid.py で生成/検証）
│   └── server.yml                   Mirakurun サーバー設定
└── src/                             バイナリの C ソースコード（make build-bins でビルド）
    ├── b61dec.c                     ACAS BS4K / BS8K デスクランブラー（ARIB STD-B61 / AES）
    ├── b21dec.c                     地上波 / BS MULTI2 デスクランブラー（ACAS 経由 / ARIB STD-B25）
    ├── tuner-stream-ng.c            地上波（ISDB-T, MPEG-TS）チューナー
    ├── tuner-stream-bs-ng.c         BS4K / BS8K（ISDB-S3）チューナー
    ├── tuner-stream-bs.c            BS（ISDB-S, MPEG-TS）チューナー（mode=1）
    └── startup.c                    Android PIE 用 _start エントリポイント
```

---

## Part 1: USB ブートでルートを取る

> **仕組みの詳細・各ファイルのビルド方法は [BOOT.md](BOOT.md) を参照してください。**

### Step 0: kernel.img を入手して展開する

ブートファイル（`initramfs_patched.uimg`）のビルドには PIX-SMB400 のカーネルイメージ `kernel.img` が必要です。
これはメーカー公式の配布ファームウェアから取得します。

**0-1. kernel.img を展開して initramfs を取り出す**

`kernel.img` を binwalk で展開すると、内部に格納された initramfs（cpio アーカイブ）が取り出せます。
取り出した cpio ファイルが、次の Step でブートイメージ（`initramfs_patched.uimg`）をビルドする元になります。

```sh
# 要件: binwalk
binwalk -e kernel.img
# → _kernel.img.extracted/988000 が initramfs の cpio アーカイブ
```

> `988000` は kernel.img 内での initramfs のオフセット（16 進）に由来するファイル名です。
> binwalk のバージョンによって抽出先ディレクトリ名やファイル名が変わることがあります。
> その場合は `_kernel.img.extracted/` 内で `file` コマンドが `ASCII cpio archive` と判定するファイルを使用してください。

---

### Step 1: ブートファイルをビルドして USB メモリを準備する

**1-1. ブートファイルをビルドする**

USB メモリに必要な 3 ファイル（`bootargs.bin` / `root_rsa_pub_crc.bin` / `initramfs_patched.uimg`）を `boot/` のスクリプトで生成します。
`initramfs_patched.uimg` のビルドには、[Step 0](#step-0-kernelimg-を入手して展開する) で取り出した initramfs cpio を使用します。

```sh
# 1) bootargs.bin / root_rsa_pub_crc.bin を生成
cd boot
docker run --rm -v "$(pwd):/usb_boot" python:3.11-slim bash -c "
pip install pycryptodome -q
cd /usb_boot && python3 make_usb_boot.py
"

# 2) Step 0 で取り出した initramfs cpio から initramfs_patched.uimg をビルド
cd ..
bash boot/build_initramfs.sh _kernel.img.extracted/988000
```

**1-2. FAT32 でフォーマットする**

```sh
sudo mkfs.fat -F 32 -n PIXBOOT /dev/sdX1
```

**1-3. USB メモリをマウントする**

```sh
sudo mkdir -p /mnt/PIXBOOT
sudo mount -o uid=$(id -u),gid=$(id -g) /dev/sdX1 /mnt/PIXBOOT
sudo chown $USER:$USER /mnt/PIXBOOT
```

**1-4. ブートファイルを USB メモリにコピーする**

```sh
# 手動でコピー
cp boot/bootargs.bin           /mnt/PIXBOOT/
cp boot/root_rsa_pub_crc.bin   /mnt/PIXBOOT/
cp boot/initramfs_patched.uimg /mnt/PIXBOOT/
```

USB メモリのルートに以下の 3 ファイルが置かれていれば OK です:

```
PIXBOOT/
├── bootargs.bin
├── root_rsa_pub_crc.bin
└── initramfs_patched.uimg
```

アンマウントして USB メモリを取り出します。

```sh
sudo umount /mnt/PIXBOOT
```

---

### Step 2: PIX-SMB400 を USB ブートで起動する

**2-1. 事前準備**

- PIX-SMB400 の電源を切る
- USB Boot ピン（基板上）にアクセスできる状態にする
- USB メモリを PIX-SMB400 の USB ポートに挿入する
- LAN ケーブルを接続する

**2-2. USB ブートピンをショートしながら電源を投入する**

1. USB Boot ピンをショートした状態を維持しながら電源を入れる
   <img width="1575" height="1181" alt="image" src="https://github.com/user-attachments/assets/09c1fc0b-332a-4d23-bf2b-5097678871e6" />

3. BootROM が USB メモリを検出し、外部 RSA 鍵で検証を行う
4. カスタム initramfs でシステムが起動する

---

### Step 3: ADB で接続確認する

起動後 30 秒ほど待ってから接続します。

**3-1. デバイスの IP アドレスを確認する**

デバイスが DHCP で取得した IP アドレスをシリアルコンソールまたは arp-scan 等で確認します。
シリアルコンソールで確認する場合、 `init_pix_netdbg.sh` が `PIXDBG: *** ADB: adb connect <デバイスのIPアドレス>:5555 ***` をカーネルログに出力しています。

**3-2. ADB 接続**

```sh
adb connect <デバイスのIPアドレス>:5555
adb -s <デバイスのIPアドレス>:5555 shell id
# → uid=0(root) gid=0(root) groups=0(root),1004(input),1007(log),1011(adb),1015(sdcard_rw),1028(sdcard_r),3001(net_bt_admin),3002(net_bt),3003(inet),3006(net_bw_stats),3009(readproc) context=u:r:su:s0
```

`uid=2000(shell)` と表示された場合は eMMC から通常ブートしています（USB ブートを再確認）。

---

## Part 2: Mirakurun のセットアップ

> セットアップ以降は USB メモリを挿入して電源を入れるだけで、`mirakurun_proxy` サービスが起動時に Mirakurun を自動起動します（`make start` は不要）。
> ただし、安全のためクラッシュ時の自動再起動はしません。
> 停止した場合は `make start` か再起動で復帰してください。

---

### Step 4: Alpine Linux + Node.js をセットアップする

デバイスにインターネット接続が必要です。コマンドはリポジトリのルートで実行します。

```sh
make setup-runtime ADB_TARGET=<デバイスのIPアドレス>:5555
```

Alpine ARM32 minirootfs のダウンロードと Node.js のインストールを自動で行います。
完了まで 3〜5 分かかります。

完了確認:

```sh
adb -s <デバイスのIPアドレス>:5555 shell \
  "chroot /data/local/tmp/mirakurun-root /bin/sh -c \
   'export PATH=/usr/sbin:/usr/bin:/sbin:/bin; node --version'"
# → v20.x.x などが返ること
```

---

### Step 5: Mirakurun をデプロイする

PC 側で Mirakurun をビルドしてからデバイスに転送します。

```sh
# リポジトリのルートに tmp/ を作ってクローン・ビルド
git clone https://github.com/nanamitm/Mirakurun-BS4K.git tmp/Mirakurun-BS4K
cd tmp/Mirakurun-BS4K && npm install && npm run build && cd ../..

# デバイスにデプロイ
make deploy-mirakurun ADB_TARGET=<デバイスのIPアドレス>:5555
```

Mirakurun のコードと設定ファイルがデバイスの `/data/local/tmp/mirakurun/` にコピーされます。

---

### Step 6: バイナリをビルドしてデプロイする

**6-1. バイナリをビルドする**

`src/` の C ソースからバイナリ（`tuner-stream-bs-ng` / `b61dec` / `tuner-stream-bs` / `b21dec`）をビルドします。
リンクには実機の Android システムライブラリが必要なため、 `make build-bins` が ADB 経由で自動取得します（デバイスが USB ブート中であること）。

```sh
# 要件: sudo apt install gcc-arm-linux-gnueabi libssl-dev
make build-bins ADB_TARGET=<デバイスのIPアドレス>:5555
```

**6-2. バイナリ・スクリプト・設定をデプロイする**

```sh
make push-all ADB_TARGET=<デバイスのIPアドレス>:5555
```

以下をデバイスにコピーします:
- `bin/tuner-stream-ng` — 地上波（ISDB-T）チューナー
- `bin/tuner-stream-bs-ng` — BS4K / BS8K チューナー
- `bin/b61dec` — BS4K / BS8K デスクランブラー（ACAS / AES）
- `bin/tuner-stream-bs` — BS チューナー（mode=1）
- `bin/b21dec` — 地上波 / BS デスクランブラー（ACAS 経由 / MULTI2）
- `scripts/*.sh` — 各種スクリプト
- `config/*.yml` — Mirakurun 設定

> 設定やスクリプトを更新した場合は `make push-all` だけ再実行します。
> バイナリ自体を変更した場合は `make build-bins` から実行します。

---

### Step 7: ACAS マスターキーを設定する

BS4K デスクランブルには ACAS マスターキー（64 文字 hex）が必要です。

```sh
# デバイスのシェルで実行（64HEX_KEY を実際のキーに置き換える）
adb -s <デバイスのIPアドレス>:5555 shell

echo '64HEX_KEY' > /data/local/tmp/.acas_key
chmod 600 /data/local/tmp/.acas_key

# 確認（65 = 64 文字 + 改行）
wc -c /data/local/tmp/.acas_key
```

---

## Part 3: 起動・確認

### Step 8: Mirakurun を起動する

```sh
make start ADB_TARGET=<デバイスのIPアドレス>:5555
```

正常起動時:

```
{"current":"4.0.0-...","latest":"..."}
```

---

### Step 9: BS4K ストリームを確認する

```sh
make test ADB_TARGET=<デバイスのIPアドレス>:5555
```

出力例（正常）:

```
Streaming BS4K 45168 for 5s...
0000000 7f 02 00 60 60 00 00 00 ...
```

- `7f 02 ...` または `7f 03 ...` → **正常**（IPv4 / IPv6 TLV コンテンツ）
- `7f ff 00 00 ...` → 未復号（b61dec の ACAS 認証失敗）→ Step 7 を確認
- 出力なし → チューナーが応答していない → `make log` でログを確認

### BS8Kの受信について

BS8K（左旋・ISDB-S3）にも対応しています。`config/channels.yml` には次のエントリが含まれています。

| name | type | channel | serviceId |
|------|------|---------|-----------|
| BS8K NHK | BS4K | 45280 | 102 |

- BS8K は右旋ではなく**左旋**で送出されるため、`smb400-tuner.sh` が channel `45280`（実 tlvStreamId）を IF `2472MHz` にマップして受信します。
  `type` は BS4K と同じ ISDB-S3 経路のため `BS4K` のままです。
- 復号経路は BS4K と共通で、`b61dec` による ACAS デスクランブルで復号されます。
- ストリーム確認:

```sh
curl -s --max-time 8 "http://<デバイスのIPアドレス>:40772/api/channels/BS4K/45280/stream" \
  | od -v -t x1 | head -4
# 先頭が 7f 02 / 7f 03 なら正常（復号済み TLV）。サービス名は「ＮＨＫ　ＢＳ８Ｋ」(serviceId 102)
```

- 視聴も BS4K と同じく mmt/tlv 対応 FFmpeg で行えます:

```sh
ffplay http://<デバイスのIPアドレス>:40772/api/channels/BS4K/45280/stream
```

> BS8K は 7680×4320 / HEVC 10bit のため、再生・録画には相応の処理能力を持つ視聴環境が必要です。

### 地上波（ISDB-T / GR）の受信について

地上波デジタル（ISDB-T）にも対応しています。
地デジも BS と同じ **MULTI2**（B-CAS 方式, CA_system_id 0x0005）でスクランブルされているため、`smb400-tuner.sh` 内の **`b21dec`** がオンデバイス ACAS チップ経由でそのまま解除します（B-CAS カード不要）。
チューナーは `tuner-stream-ng`（DMX 直接キャプチャ）を使い、`tuner-stream-ng | b21dec` を chroot 配下で実行して平文 MPEG-TS を出力します（Mirakurun 標準 TSFilter で処理）。

`config/channels.yml` の GR 一覧は **関東（東京）の物理チャンネル例**です。
物理チャンネル割り当ては地域で異なるため、お住まいの地域に合わせて `channel`（13〜62）を変更してください。
`serviceId` は省略してあり、Mirakurun のサービススキャンが各局を自動登録します。

| name | type | channel(物理) |
|------|------|---------------|
| NHK総合 | GR | 27 |
| NHK Eテレ | GR | 26 |
| 日テレ | GR | 25 |
| テレビ朝日 | GR | 24 |
| TBS | GR | 22 |
| テレビ東京 | GR | 23 |
| フジテレビ | GR | 21 |
| TOKYO MX | GR | 16 |

```sh
# 物理チャンネル 27（関東 NHK総合）を受信して先頭を確認
curl -s --max-time 8 "http://<デバイスのIPアドレス>:40772/api/channels/GR/27/stream?decode=0" \
  -o gr27.ts
ffprobe gr27.ts        # mpeg2video が見えれば復号成功
```


### BS（ISDB-S）の受信について

NHK BS などの**BS（ISDB-S, MPEG-TS）**にも対応しています。
2K BS は ARIB STD-B25 系の **MULTI2**（B-CAS 方式, CA_system_id 0x0005）でスクランブルされており、`smb400-tuner.sh` 内の **`b21dec`** がオンデバイスの **ACAS チップ**経由で解除します（B-CAS カード不要）。
BS4K の `b61dec`（ACAS-RMP / AES）とは別系統で、ACAS チップの**従来 CAS 機能**（APDU を ACAS モード P2=0x02 で送出）を使い、ECM から MULTI2 スクランブル鍵を取得します。

`config/channels.yml` には例として NHK BS（BS15 / IF 1318000kHz / tsId 16625）が含まれます。

| name | type | channel | serviceId |
|------|------|---------|-----------|
| NHK BS       | BS | BS15_0 | 101 |
| NHK BS (102) | BS | BS15_0 | 102 |
| NHK BS (103) | BS | BS15_0 | 103 |

- channel は `BSxx_y`（xx=トランスポンダ番号, y=ストリーム）形式で、`smb400-tuner.sh` が IF = `1049480 + (xx-1)/2 × 38360` kHz を算出して `tuner-stream-bs`（mode=1）でロックします。
- `b21dec` は **ACAS マスターキー不要**です（放送局のワークキー Kw は、過去の実放送受信時に EMM 経由でチップへ書き込まれた契約情報を利用するため）。
  逆に、当該局の契約・受信履歴が無いチップでは ECM 応答が「視聴不可」となり復号できません。
- 出力は平文 MPEG-TS なので Mirakurun の標準 TSFilter で処理されます（`tlvDecoder` 不要）。
- ストリーム確認・視聴:

```sh
curl -s --max-time 8 "http://<デバイスのIPアドレス>:40772/api/services/<serviceId>/stream" -o nhkbs.ts
ffprobe nhkbs.ts          # mpeg2video 1440x1080 + aac が見えれば復号成功
ffplay  "http://<デバイスのIPアドレス>:40772/api/services/<serviceId>/stream"
```

> 2K BS は受信できるトランスポンダ・サービスが地域/契約により異なります。
> `config/tuners.yml` で `BS` タイプが有効になっている必要があります（本リポジトリでは有効化済み）。

#### BS TSID 表（`config/bs_tsid.conf`）とスキャン

BS は 1 トランスポンダに複数 TS が多重されており、ハードのチューナー（`tunertest`）は**実 MPEG TS-ID 単位**で TS を選びます（TSMF 多重ストリームは出力されません）。そのため `smb400-tuner.sh` は `BSxx_y` → 実 TS-ID の対応表が必要です。この表は **`config/bs_tsid.conf`**（外部ファイル）に切り出してあり、編集すればスクリプト本体を触らずに追加・変更できます（ファイルが無い/該当行が無い場合はスクリプト内蔵の既定表へフォールバック）。

TS-ID とトランスポンダの割当は全国標準で滅多に変わりませんが、再編（中継器移動など）が起きたら実機 NIT から表を生成/検証できます:

```sh
# チューナー排他のため、視聴/EPG 取得を止めてから実行すること
make scan-bs-tsid            # 差分レポート + 候補 config/bs_tsid.conf.scanned を出力
make scan-bs-tsid WRITE=1    # config/bs_tsid.conf を実際に更新
```

1 回の BS 受信で NIT に全トランスポンダ・全 TS が列挙されるため、単一受信で全表を検証できます。ツールは既存の `BSxx_y` ラベルを保持したまま、**NEW**（新局）/ **MOVED**（TS-ID が別トランスポンダへ移動）/ **MISSING**（表にあるが受信されず）を報告します。`MOVED` / `NEW` の際は `config/channels.yml` の `channel` 名も併せて更新してください（IF は `BSxx` 名から算出されるため）。

### CS（110度CS）の受信について

110度CS（ISDB-S, MPEG-TS）にも対応しています。
BS(2K) と同じ **MULTI2**（B-CAS 方式, CA_system_id 0x0005）でスクランブルされており、`smb400-tuner.sh` 内の **`b21dec`** がオンデバイスの ACAS チップ経由で解除します（B-CAS カード不要、BS(2K) と共通の経路）。

`config/channels.yml` には ND02〜ND24 の全12トランスポンダが含まれます。

| channel | IF (kHz) |
|---------|----------|
| ND02 | 1613000 |
| ND04 | 1653000 |
| ND06 | 1693000 |
| ND08 | 1733000 |
| ND10 | 1773000 |
| ND12 | 1813000 |
| ND14 | 1853000 |
| ND16 | 1893000 |
| ND18 | 1933000 |
| ND20 | 1973000 |
| ND22 | 2013000 |
| ND24 | 2053000 |

- channel は `NDxx`（xx=偶数のトランスポンダ番号）形式で、`smb400-tuner.sh` が IF = `1613000 + (xx-2)/2 × 40000` kHz を算出して `tuner-stream-bs`（mode=1, streamId=0=auto）でロックします。
- BS(2K) と異なり、1トランスポンダ = 単一 TS に複数サービスが通常の MPTS として多重されているため、BS のような TSID 対応表は不要です（`streamId=0` の自動選択でそのまま受かります）。
- `serviceId` は省略してあり、Mirakurun のサービススキャンが各局を自動登録します。
- 実機 IF スキャン（2026-07-06）で ND02〜ND24 全12トランスポンダのロック・復号（AXN・BBC・CNNj・Mnet・MusicJapan・LaLaTV・QVC・Dlife・SKY STAGE・TBSチャンネル・日テレNEWS24・GAORA・MONDO TV 等）を確認済みです（[sun-ele.co.jp のCS-IF表](https://sun-ele.co.jp/support/how/bs_freq.html)と一致）。
- ストリーム確認・視聴は BS(2K) と同様です:

```sh
curl -s --max-time 8 "http://<デバイスのIPアドレス>:40772/api/services/<serviceId>/stream" -o cs.ts
ffprobe cs.ts
ffplay  "http://<デバイスのIPアドレス>:40772/api/services/<serviceId>/stream"
```

> CS は受信できるトランスポンダ・サービスが契約により異なります。
> `config/tuners.yml` で `CS` タイプが有効になっている必要があります（本リポジトリでは有効化済み）。

### ffplay でリアルタイム視聴

再生にはmmt/tlvに対応した [FFmpeg](https://github.com/superfashi/FFmpeg) が必要です。

```sh
ffplay http://<デバイスのIPアドレス>:40772/api/channels/BS4K/45168/stream
```

### EPGStation で録画・視聴

BS4K に対応した EPGStation フォークを使うと、Web UI から録画予約・視聴が行えます。

- リポジトリ: [tsuyopon123/EPGStation](https://github.com/tsuyopon123/EPGStation)

EPGStation をセットアップする際に `mirakurunPath` を `http://<デバイスのIPアドレス>:40772/` に設定してください。

---

## サービス登録と EPG について

初回起動後、Mirakurun は `channels.yml` で `serviceId` を指定した各チャンネルを順次チューニングしてサービスを自動登録します。
**全チャンネルが揃うまで数分**かかります（1 チューナーで順番にチューニングするため）。

```sh
# 登録されたサービスを確認
curl -s http://<デバイスのIPアドレス>:40772/api/services | python3 -m json.tool
```

- サービスが登録されると、それを対象に **EPG Gatherer / Service Updater** が動き出します（登録サービスが 0 件のうちは、これらのジョブは対象が無いため即終了します）。
- トランスポンダの初回チューニング時、ウォームアップで稀にサービスを取り逃すことがあります（ストリーム先頭の `7f ff`）。
  その場合は `make start` で再起動すれば取得されます（登録済みサービスは DB から復元されるため再チューニングされません）。

> **再起動について**: Web UI の Restart ボタンはこの構成（pm2／Docker なし）では使えず 500 を返します。
> 再起動はホストから `make restart` を使ってください。

---

## コマンド

```sh
make start    # Mirakurun 起動
make stop     # 停止（チューナー・デスクランブラーも含む）
make restart  # 再起動
make log      # ログ確認（最新 50 行）
make test     # BS4K 疎通テスト
make push-all # バイナリ・スクリプト・設定を更新

# デバイスの IP アドレスを指定する場合
make start ADB_TARGET=192.168.1.100:5555
```

---

## 注意事項

### OEM サービスと ACAS 競合

OEM チューナーサービス（`pix_airtuner`）が起動中は ACAS を占有するため、
`b61dec` が失敗します（`GetCkc failed: -4`）。

`start_mirakurun.sh` は起動時に自動で停止します。
再起動後に OEM サービスが復帰した場合は手動で停止:

```sh
adb -s <デバイスのIPアドレス>:5555 shell "stop pix_airtuner; stop airtuner; stop airtuner_4k"
```

### メモリ管理（crash_guard）

`crash_guard.sh` が自動起動し以下を監視します:

- `crash_dump32` フォーク爆弾（Android 8 のクラッシュダンプ暴走）
- MemAvailable < 600 MB → `stop_android_tv.sh` で Android TV アプリを回収（2 分クールダウン）
- MemAvailable < 350 MB → Node.js を強制終了（最終手段）

Node.js のヒープは `--max-old-space-size=256` で制限されています。

### USB Boot ファイルのビルド

`bootargs.bin` / `root_rsa_pub_crc.bin` / `initramfs_patched.uimg` のビルド手順・
オーバーレイのカスタマイズ方法は [BOOT.md](BOOT.md) を参照してください。

---

## トラブルシューティング

```sh
# ログ確認
make log ADB_TARGET=<デバイスのIPアドレス>:5555

# crash_guard ログ
adb -s <デバイスのIPアドレス>:5555 shell "tail -20 /data/local/tmp/crash_guard.log"

# メモリ確認
adb -s <デバイスのIPアドレス>:5555 shell "grep MemAvailable /proc/meminfo"
```
