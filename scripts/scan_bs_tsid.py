#!/usr/bin/env python3
"""
scan_bs_tsid.py — 実機の NIT から BS TSID 対応表 (config/bs_tsid.conf) を生成/検証する。

やっていること:
  1. SMB400 の空きチューナーで BS トランスポンダを 1 つ受信し、TS を host へ流す。
  2. NIT (PID 0x0010, table_id 0x40) を組み立てる。BS の NIT は 1 回の受信で
     全トランスポンダ・全 TS を列挙する（satellite_delivery_system_descriptor の
     周波数 + transport_stream_id）ので、単一受信で完全なネットワークマップが得られる。
  3. 周波数 → BSxx トランスポンダ番号を算出 (IF=freq-10678000kHz, TP=1+2*(IF-1049480)/38360)。
  4. 既存 config/bs_tsid.conf のラベル (BSxx_y) を「真」として保持しつつ、NIT の実測と
     照合して差分を報告する:
       - NEW    : NIT にあるが表に無い TSID（新局）
       - MOVED  : 同じ TSID が別トランスポンダへ移動（再編。要 channels.yml 更新）
       - MISSING: 表にあるが今回 NIT で見えなかった TSID（休止/廃止の可能性）
  5. 既存ラベルを保った候補ファイルを出力 (--write で config/bs_tsid.conf を上書き)。

注意: _y(相対TS番号)はアルゴリズムで完全復元できない(実機で BS15 が低ニブル規則の
      反例)。よって既存ラベルは決して機械的に付け替えず保持し、新規 TSID にのみ
      暫定ラベル (BSxx_<TSID下位ニブル>) を割り当てて REVIEW フラグを立てる。

Usage:
  python3 scripts/scan_bs_tsid.py [--target <adb-serial>] [--write] [--timeout 40]
"""
import argparse
import collections
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONF_PATH = os.path.join(REPO, "config", "bs_tsid.conf")

# BS の LNB LO = 10.678 GHz。IF(kHz) = 衛星周波数(kHz) - 10678000。
LNB_LO_KHZ = 10678000
# BS トランスポンダ TP1 の IF と TP 間隔（smb400-tuner.sh と一致させること）。
IF_TP1_KHZ = 1049480
IF_STEP_KHZ = 38360  # TP が 2 増えるごとの IF 差

# NIT 取得に試す候補トランスポンダ (BSxx)。どれか 1 つ受かれば全ネットワーク分の NIT が取れる。
SCAN_CANDIDATE_TPS = [15, 1, 13, 3, 21]

TUNER_STREAM_BS = "/data/local/tmp/tuner-stream-bs"


def if_khz_for_tp(tp):
    return IF_TP1_KHZ + (tp - 1) // 2 * IF_STEP_KHZ


def tp_for_freq_khz(freq_khz):
    """衛星周波数(kHz) → BS トランスポンダ番号。割り切れなければ None。"""
    if_khz = freq_khz - LNB_LO_KHZ
    num = (if_khz - IF_TP1_KHZ)
    if num % IF_STEP_KHZ != 0:
        # 近い値に丸めて許容（受信誤差ではなく BCD 由来なので通常はぴったり）
        tp = round(num / IF_STEP_KHZ) * 2 + 1
    else:
        tp = num // IF_STEP_KHZ * 2 + 1
    return tp if tp >= 1 else None


# ---------------------------------------------------------------------------
# ADB
# ---------------------------------------------------------------------------
def adb_base(target):
    binname = "adb.exe" if shutil.which("adb.exe") else "adb"
    cmd = [binname]
    if target:
        cmd += ["-s", target]
    return cmd


def adb_shell(target, script):
    return subprocess.run(adb_base(target) + ["shell", script],
                          capture_output=True, text=True).stdout.strip()


def tuner_busy(target):
    out = adb_shell(target, "pgrep tunertest || true")
    return bool(out.strip())


def kill_tuner(target):
    adb_shell(target, "pkill -9 tunertest 2>/dev/null; pkill -9 -f tuner-stream-bs 2>/dev/null; true")


# ---------------------------------------------------------------------------
# TS / NIT parsing
# ---------------------------------------------------------------------------
class SectionAsm:
    """PID 0x0010 の PSI セクションを PUSI/section_length に従って組み立てる。"""
    def __init__(self, pid):
        self.pid = pid
        self.buf = b""
        self.collecting = False

    def feed_packet(self, pkt):
        """完成したセクションを list で返す。"""
        pid = ((pkt[1] & 0x1f) << 8) | pkt[2]
        if pid != self.pid:
            return []
        pusi = pkt[1] & 0x40
        afc = (pkt[3] >> 4) & 3
        idx = 4
        if afc & 2:
            idx += 1 + pkt[4]
        if idx >= 188:
            return []
        payload = pkt[idx:]
        if pusi:
            if not payload:
                return []
            ptr = payload[0]
            # pointer_field 直後までは「前のセクションの続き」
            if self.collecting:
                self.buf += payload[1:1 + ptr]
            out = self._extract_complete()
            # pointer_field 以降が新しいセクションの先頭
            self.buf = payload[1 + ptr:]
            self.collecting = True
            return out + self._extract_complete()
        elif self.collecting:
            self.buf += payload
            return self._extract_complete()
        return []

    def _extract_complete(self):
        res = []
        while len(self.buf) >= 3:
            if self.buf[0] == 0xff:
                self.buf = b""
                break
            seclen = ((self.buf[1] & 0x0f) << 8) | self.buf[2]
            total = 3 + seclen
            if len(self.buf) < total:
                break
            res.append(self.buf[:total])
            self.buf = self.buf[total:]
        return res


def parse_nit_section(sec):
    """table_id 0x40 の NIT セクションを (tsid, onid, freq_khz) のリストに。0x40 以外は None。"""
    if len(sec) < 10 or sec[0] != 0x40:
        return None
    last_section = sec[7]
    section_number = sec[6]
    q = 8
    net_desc_len = ((sec[q] & 0x0f) << 8) | sec[q + 1]; q += 2
    q += net_desc_len
    if q + 2 > len(sec):
        return None
    ts_loop_len = ((sec[q] & 0x0f) << 8) | sec[q + 1]; q += 2
    end = min(q + ts_loop_len, len(sec) - 4)  # 末尾4byteはCRC
    rows = []
    while q + 6 <= end:
        tsid = (sec[q] << 8) | sec[q + 1]
        onid = (sec[q + 2] << 8) | sec[q + 3]
        td_len = ((sec[q + 4] & 0x0f) << 8) | sec[q + 5]
        dp = q + 6; dend = dp + td_len
        freq_khz = None
        while dp + 2 <= dend and dp + 2 <= len(sec):
            dtag = sec[dp]; dlen = sec[dp + 1]
            if dtag == 0x43 and dlen >= 4:  # satellite_delivery_system_descriptor
                bcd = sec[dp + 2:dp + 6].hex()  # BCD: 各ニブルが10進桁
                try:
                    freq_khz = int(bcd) * 10  # 単位 10kHz → kHz
                except ValueError:
                    freq_khz = None
            dp += 2 + dlen
        rows.append((tsid, onid, freq_khz))
        q += 6 + td_len
    return {"section": section_number, "last": last_section, "rows": rows}


def stream_and_collect_nit(target, timeout):
    """候補 TP を順に受信し、完全な NIT を組み立てて {tsid: freq_khz} を返す。"""
    for tp in SCAN_CANDIDATE_TPS:
        if_khz = if_khz_for_tp(tp)
        print(f"[*] BS{tp:02d} (IF {if_khz} kHz) を受信して NIT 取得を試行...", file=sys.stderr)
        cmd = adb_base(target) + ["exec-out",
              f"chroot /proc/1/root /system/bin/sh -c "
              f"'{TUNER_STREAM_BS} 0 1 {if_khz} 0 2>/dev/null'"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        asm = SectionAsm(0x0010)
        by_section = {}
        last_section = None
        carry = b""
        deadline = time.time() + timeout
        result = None
        try:
            while time.time() < deadline:
                chunk = proc.stdout.read(188 * 256)
                if not chunk:
                    break
                data = carry + chunk
                n = len(data) - (len(data) % 188)
                # 同期がずれる場合に備え 0x47 で軽く整列
                for i in range(0, n, 188):
                    if data[i] != 0x47:
                        continue
                    for sec in asm.feed_packet(data[i:i + 188]):
                        parsed = parse_nit_section(sec)
                        if parsed is None:
                            continue
                        by_section[parsed["section"]] = parsed["rows"]
                        last_section = parsed["last"]
                if last_section is not None and \
                        all(s in by_section for s in range(last_section + 1)):
                    result = by_section
                    break
                carry = data[n:]
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
            kill_tuner(target)
        if result:
            tsid_freq = {}
            for rows in result.values():
                for tsid, onid, freq_khz in rows:
                    if onid == 4 and freq_khz:  # BS: original_network_id=4
                        tsid_freq[tsid] = freq_khz
            if tsid_freq:
                print(f"[+] NIT 取得成功: {len(tsid_freq)} TS を検出", file=sys.stderr)
                return tsid_freq
        print(f"[-] BS{tp:02d}: 完全な NIT を取得できず、次の候補へ", file=sys.stderr)
    return None


# ---------------------------------------------------------------------------
# config/bs_tsid.conf reconciliation
# ---------------------------------------------------------------------------
def load_conf(path):
    """{tsid: label} と行順を返す。"""
    mapping = {}
    if not os.path.exists(path):
        return mapping
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if len(parts) >= 2 and parts[0].startswith("BS"):
                try:
                    mapping[int(parts[1])] = parts[0]
                except ValueError:
                    pass
    return mapping


def label_tp(label):
    """'BS15_0' -> 15"""
    try:
        return int(label[2:label.index("_")])
    except (ValueError, IndexError):
        return None


def reconcile(tsid_freq, current):
    """NIT 実測と既存表を突き合わせ、(entries, reports) を返す。
    entries: [(label, tsid, tp)] 出力用（既存ラベル保持）。"""
    cur_by_tsid = dict(current)  # tsid -> label
    entries = []
    reports = []  # (kind, message)
    used_labels = set(cur_by_tsid.values())

    # トランスポンダごとに、その TP で既に使われている _y を把握
    def next_label(tp):
        used = {lbl for lbl in used_labels if label_tp(lbl) == tp}
        i = 0
        while f"BS{tp:02d}_{i}" in used:
            i += 1
        return f"BS{tp:02d}_{i}"

    for tsid in sorted(tsid_freq):
        tp = tp_for_freq_khz(tsid_freq[tsid])
        if tp is None:
            reports.append(("WARN", f"TSID {tsid}: 周波数 {tsid_freq[tsid]}kHz から TP を算出できず。スキップ"))
            continue
        if tsid in cur_by_tsid:
            label = cur_by_tsid[tsid]
            cur_tp = label_tp(label)
            if cur_tp != tp:
                # 同じ TSID が別トランスポンダへ移動 = 再編
                reports.append(("MOVED",
                    f"TSID {tsid} が {label}(BS{cur_tp:02d}) → BS{tp:02d} へ移動。"
                    f"ラベルを BS{tp:02d}_y に変更し channels.yml も更新すること"))
                # 移動先の暫定ラベルで出力
                label = next_label(tp)
                used_labels.add(label)
            entries.append((label, tsid, tp))
        else:
            # 新規 TSID：下位ニブルを暫定 _y に（多くの TP で一致。要レビュー）
            suggest = f"BS{tp:02d}_{tsid & 0x0f}"
            if suggest in used_labels:
                suggest = next_label(tp)
            used_labels.add(suggest)
            entries.append((suggest, tsid, tp))
            reports.append(("NEW",
                f"新 TSID {tsid} @ BS{tp:02d} → 暫定ラベル {suggest} を割当。"
                f"_y が正しいか確認し channels.yml に追加すること"))

    # 表にあるが NIT で見えなかったもの
    seen = set(tsid_freq)
    for tsid, label in current.items():
        if tsid not in seen:
            reports.append(("MISSING",
                f"TSID {tsid} ({label}) が今回 NIT に無し（休止/廃止/受信不良の可能性）。表には残す"))
            tp = label_tp(label)
            entries.append((label, tsid, tp if tp else 0))

    # ラベル順（TP → _y）で整列
    def sort_key(e):
        lbl = e[0]
        tp = label_tp(lbl) or 0
        try:
            y = int(lbl[lbl.index("_") + 1:])
        except (ValueError, IndexError):
            y = 0
        return (tp, y)
    entries.sort(key=sort_key)
    return entries, reports


def render_conf(entries):
    lines = [
        "# bs_tsid.conf — BS (2K / ISDB-S) チャンネル → 実 MPEG TS-ID 対応表",
        "#",
        "# scan_bs_tsid.py が実機 NIT から生成/更新。書式: <CHANNEL> <TSID>",
        "# CHANNEL は channels.yml の channel と一致させること。IF は BSxx 名から算出。",
        "# （詳細な運用注意は git 履歴/README を参照）",
        f"# generated: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        "",
    ]
    for label, tsid, tp in entries:
        lines.append(f"{label} {tsid}")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description="実機 NIT から BS TSID 表を生成/検証")
    ap.add_argument("--target", help="adb シリアル (省略時は既定デバイス)")
    ap.add_argument("--write", action="store_true",
                    help="config/bs_tsid.conf を実際に上書きする（既定は候補ファイル出力のみ）")
    ap.add_argument("--timeout", type=int, default=40,
                    help="1トランスポンダあたりの NIT 取得タイムアウト秒 (既定40)")
    args = ap.parse_args()

    if tuner_busy(args.target):
        print("[!] チューナーが使用中です (tunertest 稼働中)。視聴/EPG を止めてから再実行してください。",
              file=sys.stderr)
        return 2

    tsid_freq = stream_and_collect_nit(args.target, args.timeout)
    if not tsid_freq:
        print("[!] NIT を取得できませんでした。アンテナ接続・信号を確認してください。", file=sys.stderr)
        return 1

    current = load_conf(CONF_PATH)
    entries, reports = reconcile(tsid_freq, current)

    print("\n===== 差分レポート =====")
    order = {"MOVED": 0, "NEW": 1, "MISSING": 2, "WARN": 3}
    changes = [r for r in reports if r[0] in ("MOVED", "NEW", "MISSING", "WARN")]
    if not changes:
        print("既存 config/bs_tsid.conf と実機 NIT は一致。変更なし。")
    else:
        for kind, msg in sorted(reports, key=lambda r: order.get(r[0], 9)):
            print(f"  [{kind}] {msg}")

    new_conf = render_conf(entries)
    if args.write:
        with open(CONF_PATH, "w") as f:
            f.write(new_conf)
        print(f"\n[+] {CONF_PATH} を更新しました ({len(entries)} エントリ)。")
        if any(k in ("MOVED", "NEW") for k, _ in reports):
            print("[!] MOVED/NEW があります。channels.yml の channel 名も忘れず更新してください。")
    else:
        out = CONF_PATH + ".scanned"
        with open(out, "w") as f:
            f.write(new_conf)
        print(f"\n[+] 候補を {out} に出力しました ({len(entries)} エントリ)。")
        print("    差分を確認のうえ問題なければ --write で上書き、または手動で反映してください。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
