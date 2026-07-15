#!/usr/bin/env bash
#
# build-efi.sh —— 編譯 UsbOcRecover.efi
#
#   前提：先跑過 setup-edk2-ubuntu.sh。
#   裝好之後在任何目錄打 `ocbuild` 就等於跑這支。
#
# 用法：
#   ocbuild                 RELEASE 版
#   ocbuild DEBUG           DEBUG 版（檔案小很多，原因見下方註解）
#   ocbuild --clean         清掉 Build 目錄重編
#
# 環境變數：
#   EDK2_DIR=$HOME/edk2     edk2 在哪
#   TOOLCHAIN=GCC5          工具鏈
#
set -uo pipefail

EDK2_DIR="${EDK2_DIR:-$HOME/edk2}"
TOOLCHAIN="${TOOLCHAIN:-GCC5}"
# edk2 BaseTools 對非 ASCII 路徑會出問題，在 WSL 上直接編 /mnt/c 也慢到爆，
# 所以一律先複製到 Linux 檔案系統上的乾淨英文路徑再編。
WORK_DIR="${WORK_DIR:-$HOME/ocbuild-work}"

RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; CYN=$'\033[36m'; BLD=$'\033[1m'; RST=$'\033[0m'
step() { echo; echo "${BLD}${CYN}==> $*${RST}"; }
die()  { echo; echo "${RED}${BLD}[失敗] $*${RST}" >&2; exit 1; }

CLEAN=0
TARGET="RELEASE"
for a in "$@"; do
    case "$a" in
        --clean)        CLEAN=1 ;;
        RELEASE|release) TARGET="RELEASE" ;;
        DEBUG|debug)     TARGET="DEBUG" ;;
        *) die "不認得的參數：$a（可用：RELEASE / DEBUG / --clean）" ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

#
# 自動判斷 UsbOcRecoverPkg 在哪，讓同一份腳本支援兩種擺法：
#
#   (A) git repo（EFIVMV1）—— 腳本在根目錄，跟 package 同層
#         EFIVMV1/build-efi.sh
#         EFIVMV1/UsbOcRecoverPkg/
#
#   (B) OCV2 專案 —— 腳本在子資料夾裡
#         OCV2/ubuntu建置工具/build-efi.sh
#         OCV2/UsbOcRecoverPkg/
#
# 寫死 "$SCRIPT_DIR/.." 的話 (A) 會找不到 package。
#
if [ -f "$SCRIPT_DIR/UsbOcRecoverPkg/UsbOcRecoverPkg.dsc" ]; then
    PROJ_DIR="$SCRIPT_DIR"
elif [ -f "$SCRIPT_DIR/../UsbOcRecoverPkg/UsbOcRecoverPkg.dsc" ]; then
    PROJ_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
else
    echo "找不到 UsbOcRecoverPkg/UsbOcRecoverPkg.dsc" >&2
    echo "  找過：$SCRIPT_DIR/UsbOcRecoverPkg/" >&2
    echo "  找過：$SCRIPT_DIR/../UsbOcRecoverPkg/" >&2
    echo "這支腳本要跟 UsbOcRecoverPkg 放在一起（同層或上一層）。" >&2
    exit 1
fi
PKG_SRC="$PROJ_DIR/UsbOcRecoverPkg"

set -e

# ------------------------------------------------------------------
step "0. 檢查前置條件"
# ------------------------------------------------------------------
[ -d "$EDK2_DIR" ] || die "找不到 edk2（$EDK2_DIR）。
       先跑：$SCRIPT_DIR/setup-edk2-ubuntu.sh"
[ -x "$EDK2_DIR/BaseTools/Source/C/bin/GenFw" ] || die "BaseTools 沒編過。
       跑：make -C $EDK2_DIR/BaseTools
       或重跑：$SCRIPT_DIR/setup-edk2-ubuntu.sh"
[ -f "$PKG_SRC/UsbOcRecoverPkg.dsc" ] || die "找不到 $PKG_SRC/UsbOcRecoverPkg.dsc"
for c in gcc make python3 nasm; do
    command -v "$c" >/dev/null 2>&1 || die "$c 沒裝。跑 setup-edk2-ubuntu.sh。"
done
echo "  edk2   : $EDK2_DIR ($(git -C "$EDK2_DIR" describe --tags 2>/dev/null || echo '?'))"
echo "  專案   : $PROJ_DIR"
echo "  目標   : X64 / $TOOLCHAIN / $TARGET"

# ------------------------------------------------------------------
step "1. 複製專案到乾淨路徑"
# ------------------------------------------------------------------
rm -rf "${WORK_DIR:?}/UsbOcRecoverPkg"
mkdir -p "$WORK_DIR"
cp -r "$PKG_SRC" "$WORK_DIR/"
echo "  $PKG_SRC"
echo "    -> $WORK_DIR/UsbOcRecoverPkg"

if [ "$CLEAN" -eq 1 ]; then
    echo "  --clean：清掉舊的 Build 目錄"
    rm -rf "$WORK_DIR/Build"
fi

# ------------------------------------------------------------------
step "2. 設定 edk2 環境"
# ------------------------------------------------------------------
cd "$EDK2_DIR"
export WORKSPACE="$EDK2_DIR"

#
# ★ 這行 set -- 不能刪，刪了 `ocbuild DEBUG` 就會壞 ★
#
# source 一個腳本時，它會繼承「呼叫者的位置參數」。
# 所以 `ocbuild DEBUG` -> build-efi.sh 的 $1="DEBUG"
#   -> source ./edksetup.sh 時 edksetup 也看到 $1="DEBUG"
#   -> 它把 DEBUG 當成自己的選項，不認得，印出 help 然後 return 1
#   -> 這裡就 die 在「source edksetup.sh 失敗」，訊息完全誤導。
#
# 陰險的地方：不帶參數時（`ocbuild`）$1 是空的，edksetup 正常，一切看起來沒事。
# 只有帶參數才會炸。上面的參數已經解析進 TARGET/CLEAN 了，這裡清掉不影響。
#
set --

# edksetup.sh 內部會讀未定義的變數，開著 set -u 會被絆倒
set +u
# shellcheck disable=SC1091
source ./edksetup.sh >/dev/null || die "source edksetup.sh 失敗。"
set -u

#
# ★★ 這段是踩過坑才長這樣的，不要「順手簡化」★★
#
# 問題：`build -p UsbOcRecoverPkg/UsbOcRecoverPkg.dsc` 這個相對路徑，
#       edk2 是**先用 WORKSPACE 解析**，PACKAGES_PATH 只是後備。
#       如果 WORKSPACE=edk2，而 edk2 底下剛好也有一份舊的 UsbOcRecoverPkg
#       （手動編過的人幾乎都會有），build 就會安靜地去編那份舊的，
#       然後回報 SUCCESS。你會拿到一顆看起來對、其實是舊版的 .efi，
#       全程沒有任何錯誤訊息。這個坑實際踩過，浪費了三次 build。
#
# 解法：edksetup.sh 跑完之後，把 WORKSPACE 改指到我們的工作目錄，
#       CONF_PATH 留在 edk2（Conf 是 edksetup 產生的，在那邊）。
#       這樣 -p 一定解析到我們這次的原始碼。
#
export WORKSPACE="$WORK_DIR"
export PACKAGES_PATH="$WORK_DIR:$EDK2_DIR"
export CONF_PATH="$EDK2_DIR/Conf"

command -v build >/dev/null 2>&1 || die "source 完 edksetup.sh 還是沒有 build 指令。
       通常代表 BaseTools 沒編成功，跑：make -C $EDK2_DIR/BaseTools"

if [ -d "$EDK2_DIR/UsbOcRecoverPkg" ]; then
    echo "  ${YLW}注意：$EDK2_DIR/UsbOcRecoverPkg 也存在（可能是舊版）。${RST}"
    echo "  ${YLW}      WORKSPACE 已指到 $WORK_DIR，所以編的是這次的原始碼。${RST}"
fi
echo "  WORKSPACE     = $WORKSPACE"
echo "  PACKAGES_PATH = $PACKAGES_PATH"
echo "  CONF_PATH     = $CONF_PATH"

# ------------------------------------------------------------------
step "3. 編譯"
# ------------------------------------------------------------------
build -a X64 -t "$TOOLCHAIN" -p UsbOcRecoverPkg/UsbOcRecoverPkg.dsc -b "$TARGET" \
    || die "編譯失敗。把上面的錯誤訊息貼給我。"

# ------------------------------------------------------------------
step "4. 取出成品"
# ------------------------------------------------------------------
# Build 目錄跟著 WORKSPACE 走，所以在 WORK_DIR 底下不是 EDK2_DIR 底下
EFI_OUT="$WORK_DIR/Build/UsbOcRecoverPkg/${TARGET}_${TOOLCHAIN}/X64/UsbOcRecover.efi"
[ -f "$EFI_OUT" ] || die "build 說成功了，但找不到 $EFI_OUT"

#
# 每次都驗證產物，不是只看 build 的 exit code。
# 理由見上面 WORKSPACE 那段註解：build 回報 SUCCESS 但編到舊原始碼是真的會發生，
# 而且沒有任何錯誤訊息。這裡自動擋掉。
#
if [ -x "$SCRIPT_DIR/verify-efi.sh" ]; then
    if ! "$SCRIPT_DIR/verify-efi.sh" "$EFI_OUT" > /tmp/ocverify.$$ 2>&1; then
        cat /tmp/ocverify.$$
        rm -f /tmp/ocverify.$$
        die "產物驗證沒過（上面有原因）。編出來的東西不對，不要用。"
    fi
    rm -f /tmp/ocverify.$$
    echo "  產物驗證：${GRN}通過（確認是這份原始碼的 v2.0）${RST}"
else
    echo "  ${YLW}找不到 verify-efi.sh，跳過產物驗證。${RST}"
fi

if [ "$TARGET" = "RELEASE" ]; then
    DEST="$PROJ_DIR/UsbOcRecover_RELEASE.efi"
else
    DEST="$PROJ_DIR/UsbOcRecover.efi"
fi
cp "$EFI_OUT" "$DEST" 2>/dev/null || {
    DEST="$HOME/$(basename "$DEST")"
    cp "$EFI_OUT" "$DEST" || die "複製成品失敗。"
    echo "  ${YLW}寫不進專案目錄，改放到 $DEST${RST}"
}

echo
echo "${GRN}${BLD}================ 編譯成功 ================${RST}"
echo "  ${BLD}$DEST${RST}"
echo "  大小  : $(stat -c %s "$DEST") bytes"
echo "  類型  : $(file -b "$DEST" 2>/dev/null || echo 'PE32+ EFI application')"
#
# 檔案 ~580KB 是正常的，不是編壞了。
#
# 程式有個 512KB 的全域陣列 gLogBuffer（1024 筆 x 256 CHAR16）。它內容全是零，
# 理論上可以放進 BSS、載入時才配置，不必佔檔案空間。實際會不會佔，看工具鏈：
#
#   VS2022 DEBUG    .data 檔案=512      -> 39 KB   （唯一留成 BSS 的組合）
#   VS2022 RELEASE  .data 檔案=567296   -> 587 KB  （/MERGE:.rdata=.data 逼它寫出來）
#   GCC5   DEBUG    .data 檔案=549248   -> 582 KB
#   GCC5   RELEASE  .data 檔案=549248   -> 582 KB
#
# 所以在 Linux 上 DEBUG 和 RELEASE 大小一樣，這是對的。
# 四顆功能與 RAM 用量完全相同，差別只有檔案大小。隨身碟放哪顆都行。
#
echo
echo "  放到 USB 隨身碟（${BLD}必須是 FAT32${RST}）："
echo "      隨身碟/EFI/BOOT/BOOTX64.EFI      <- 插上去開機就直接進工具"
echo "${GRN}${BLD}==========================================${RST}"
