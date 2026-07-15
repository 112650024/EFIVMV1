#!/usr/bin/env bash
#
# verify-efi.sh —— 確認 .efi 真的是 v2.0，不是撿到舊版
#
# 為什麼需要這支：
#   edk2 的 `build` 回報 SUCCESS **不代表**編到你以為的那份原始碼。
#   如果 WORKSPACE 底下有另一份同名的 package（手動編過的人幾乎都會有），
#   build 會安靜地編那份舊的、產生 .efi、回報成功，全程零錯誤零警告。
#   實際踩過，浪費了三次 build。詳見 ../編譯踩雷紀錄.md 的「雷 1」。
#
#   所以：改版之後不要只看 exit code，要驗證產物。
#
# 用法：
#   ./verify-efi.sh                    自動找最近編出來的 .efi
#   ./verify-efi.sh path/to/xxx.efi    指定檔案
#
set -uo pipefail

RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; BLD=$'\033[1m'; RST=$'\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 兩種擺法都支援：腳本跟 package 同層（git repo）或在子資料夾裡（OCV2 專案）
if [ -f "$SCRIPT_DIR/UsbOcRecoverPkg/UsbOcRecoverPkg.dsc" ]; then
    PROJ_DIR="$SCRIPT_DIR"
else
    PROJ_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
WORK_DIR="${WORK_DIR:-$HOME/ocbuild-work}"

TARGET_FILE="${1:-}"

if [ -z "$TARGET_FILE" ]; then
    # 沒指定就找最近編出來的
    TARGET_FILE="$(find "$WORK_DIR/Build" "$PROJ_DIR" -maxdepth 6 -name 'UsbOcRecover*.efi' \
                   -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)"
    if [ -z "$TARGET_FILE" ]; then
        echo "${RED}找不到任何 UsbOcRecover*.efi。${RST}" >&2
        echo "先跑 ocbuild，或直接指定路徑：./verify-efi.sh path/to/xxx.efi" >&2
        exit 1
    fi
    echo "${YLW}（沒指定檔案，自動挑最新的）${RST}"
fi

[ -f "$TARGET_FILE" ] || { echo "${RED}找不到檔案：$TARGET_FILE${RST}" >&2; exit 1; }

command -v python3 >/dev/null 2>&1 || {
    echo "${RED}需要 python3 才能檢查（字串是 UTF-16LE，grep 不好處理）。${RST}" >&2
    exit 1
}

python3 - "$TARGET_FILE" <<'PYEOF'
import os, struct, sys

path = sys.argv[1]
b = open(path, 'rb').read()

GRN = "\033[32m"; RED = "\033[31m"; BLD = "\033[1m"; RST = "\033[0m"

def has(s):
    # EFI 程式的字串常數是 UTF-16LE（CHAR16）
    return s.encode('utf-16-le') in b

# v2.0 才有的句子
V2_ONLY = [
    "Port 詳細診斷",
    "PLS 仍為",
    "導向 RxDetect",
    "這個 port 發生了什麼事",
    "位址是怎麼算出來的",
]
# v1.1 才有、v2.0 已經拿掉的句子
V1_ONLY = [
    "開始 Polling 監控",
    "切換 Auto-Recovery",
    "列出所有 Port",
]

print()
print(f"{BLD}檢查：{path}{RST}")
print(f"  大小 : {os.path.getsize(b and path):,} bytes")

# --- PE 格式 ---
is_pe = b[:2] == b'MZ'
subsystem = None
if is_pe:
    try:
        pe = struct.unpack_from('<I', b, 0x3c)[0]
        magic = struct.unpack_from('<H', b, pe + 24)[0]
        subsystem = struct.unpack_from('<H', b, pe + 24 + 68)[0]
        arch = "x86-64" if magic == 0x20b else "32-bit"
        # 10 = EFI application
        sub_name = {10: "EFI application", 11: "EFI boot service driver",
                    12: "EFI runtime driver"}.get(subsystem, f"其他({subsystem})")
        print(f"  格式 : PE32+ {arch} / {sub_name}")
    except Exception:
        is_pe = False

print()
print(f"{BLD}v2.0 專屬字串（每個都該在）{RST}")
for s in V2_ONLY:
    ok = has(s)
    print(f"  {GRN+'有'+RST if ok else RED+'不在'+RST}  {s}")

print()
print(f"{BLD}v1.1 專屬字串（每個都不該在）{RST}")
for s in V1_ONLY:
    gone = not has(s)
    print(f"  {GRN+'已移除'+RST if gone else RED+'還在！'+RST}  {s}")

okv2 = all(has(s) for s in V2_ONLY)
nov1 = not any(has(s) for s in V1_ONLY)

print()
if is_pe and okv2 and nov1 and subsystem == 10:
    print(f"{GRN}{BLD}==> 通過：這是 v2.0，而且是合法的 EFI application{RST}")
    sys.exit(0)

print(f"{RED}{BLD}==> 沒通過{RST}")
if not is_pe:
    print("    不是 PE 檔 —— 根本不是 EFI 程式")
if subsystem is not None and subsystem != 10:
    print(f"    subsystem={subsystem}，不是 EFI application(10) —— INF 的 MODULE_TYPE 可能不對")
if not okv2:
    print("    缺 v2.0 的字串")
if not nov1:
    print("    ★ 含有 v1.1 的字串 —— 你編到舊版原始碼了")
    print("      這就是「雷 1」：WORKSPACE 底下有另一份同名 package 把你的蓋掉。")
    print("      檢查 build 輸出的 'Active Platform =' 指到哪裡。")
sys.exit(1)
PYEOF
