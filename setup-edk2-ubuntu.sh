#!/usr/bin/env bash
#
# setup-edk2-ubuntu.sh —— edk2 編譯環境一鍵安裝
#
#   在乾淨的 Ubuntu（實體機 / VMware / VirtualBox / WSL 都可以）上，
#   把編譯 UefiApplication 需要的東西全部裝好、路徑設好，
#   最後直接編一次 UsbOcRecover.efi 當作驗證。
#
# 用法：
#   ./setup-edk2-ubuntu.sh            安裝（可重複執行，已裝好的會跳過）
#   ./setup-edk2-ubuntu.sh --check    只診斷不安裝，告訴你哪一步壞了
#   ./setup-edk2-ubuntu.sh --help     說明
#
# 環境變數：
#   EDK2_TAG=edk2-stable202411    要用的 edk2 版本
#   EDK2_DIR=$HOME/edk2           edk2 裝到哪
#
set -uo pipefail

EDK2_TAG="${EDK2_TAG:-edk2-stable202411}"
EDK2_DIR="${EDK2_DIR:-$HOME/edk2}"
EDK2_REPO="https://github.com/tianocore/edk2.git"
BIN_DIR="$HOME/.local/bin"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; CYN=$'\033[36m'; BLD=$'\033[1m'; RST=$'\033[0m'
step()  { echo; echo "${BLD}${CYN}==> $*${RST}"; }
ok()    { echo "  ${GRN}[OK]${RST}   $*"; }
warn()  { echo "  ${YLW}[注意]${RST} $*"; }
fail()  { echo "  ${RED}[X]${RST}    $*"; }
die()   { echo; echo "${RED}${BLD}[失敗] $*${RST}" >&2; exit 1; }

CHECK_ONLY=0
case "${1:-}" in
  --check) CHECK_ONLY=1 ;;
  --help|-h)
      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
  "") ;;
  *) die "不認得的參數：$1（用 --help 看說明）" ;;
esac

# ==================================================================
#  診斷模式：只檢查、不動系統
# ==================================================================
if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "${BLD}edk2 環境診斷${RST}"
    problems=0

    step "系統"
    . /etc/os-release 2>/dev/null || true
    echo "  發行版 : ${PRETTY_NAME:-未知}"
    echo "  核心   : $(uname -r)"
    if grep -qi microsoft /proc/version 2>/dev/null; then
        echo "  環境   : WSL"
    elif [ -d /proc/vz ] || systemd-detect-virt -q 2>/dev/null; then
        echo "  環境   : 虛擬機（$(systemd-detect-virt 2>/dev/null || echo '?')）"
    else
        echo "  環境   : 實體機"
    fi

    step "必要指令"
    for c in gcc make git nasm iasl python3; do
        if command -v "$c" >/dev/null 2>&1; then
            ok "$c  ($(command -v "$c"))"
        else
            fail "$c  沒裝"; problems=$((problems+1))
        fi
    done

    step "edk2 原始碼"
    if [ -d "$EDK2_DIR/.git" ]; then
        ok "$EDK2_DIR 存在（版本：$(git -C "$EDK2_DIR" describe --tags 2>/dev/null || echo '?')）"
        if [ -f "$EDK2_DIR/MdePkg/MdePkg.dec" ]; then
            ok "MdePkg 在"
        else
            fail "MdePkg 不見了，clone 可能不完整"; problems=$((problems+1))
        fi
        #
        # 只檢查 brotli，不要檢查 openssl。
        #
        # BaseTools 的 C 工具裡只有 BrotliCompress 需要 submodule；
        # openssl 是 CryptoPkg 用的，編 UefiApplication（只用 MdePkg）根本用不到，
        # 而它是最大、最常抓失敗的那個。拿 openssl 當判斷標準會誤報
        # 「submodule 沒抓」，但其實你該編的都編得起來。
        #
        if [ -n "$(ls -A "$EDK2_DIR/BaseTools/Source/C/BrotliCompress/brotli" 2>/dev/null)" ]; then
            ok "submodule 已取得（brotli 在，BaseTools 可以編）"
        else
            fail "brotli 沒抓到，BaseTools 會編不過"
            echo "         修：$SCRIPT_DIR/fix-submodule.sh"
            problems=$((problems+1))
        fi
        if [ -z "$(ls -A "$EDK2_DIR/CryptoPkg/Library/OpensslLib/openssl" 2>/dev/null)" ]; then
            echo "         （openssl 沒抓，但編 UefiApplication 用不到它，不算問題）"
        fi
    else
        fail "$EDK2_DIR 不存在"; problems=$((problems+1))
    fi

    step "BaseTools（最常壞的地方）"
    if [ -x "$EDK2_DIR/BaseTools/Source/C/bin/GenFw" ]; then
        ok "BaseTools 已編譯（GenFw 在）"
    else
        fail "BaseTools 沒編 -> 跑：make -C $EDK2_DIR/BaseTools"
        problems=$((problems+1))
    fi

    step "ocbuild 指令"
    #
    # 注意：這裡不能用「PATH 裡有沒有 ocbuild」當判斷標準。
    # ~/.bashrc 對非互動式 shell 會提前 return，而 ~/.profile 只有
    # login shell 會讀 —— 所以用 `bash -c` 跑這個診斷時，PATH 本來就不會有
    # ~/.local/bin，即使一切正常。之前這裡誤報過一次「有 1 個問題」。
    # 真正該檢查的是檔案在不在。
    #
    if [ -x "$BIN_DIR/ocbuild" ]; then
        ok "ocbuild 已安裝（$BIN_DIR/ocbuild）"
        if ! command -v ocbuild >/dev/null 2>&1; then
            echo "         （目前這個 shell 的 PATH 沒有它，但你開 Ubuntu 視窗時會有。"
            echo "           若在互動式終端機也叫不到，跑：source ~/.bashrc）"
        fi
    else
        fail "ocbuild 沒安裝"; problems=$((problems+1))
    fi

    step "磁碟空間"
    avail_kb=$(df -Pk "$HOME" | awk 'NR==2{print $4}')
    avail_gb=$((avail_kb / 1024 / 1024))
    if [ "$avail_gb" -ge 15 ]; then
        ok "$HOME 還有 ${avail_gb} GB"
    else
        fail "$HOME 只剩 ${avail_gb} GB，edk2 建議留 15 GB 以上"
        problems=$((problems+1))
    fi

    echo
    if [ "$problems" -eq 0 ]; then
        echo "${GRN}${BLD}==> 環境正常，可以直接跑 ocbuild${RST}"
        exit 0
    else
        echo "${RED}${BLD}==> 有 $problems 個問題。跑 ./setup-edk2-ubuntu.sh（不加參數）可以自動修。${RST}"
        exit 1
    fi
fi

# ==================================================================
#  安裝模式
# ==================================================================
set -e

echo "${BLD}=========================================${RST}"
echo "${BLD}  edk2 編譯環境安裝${RST}"
echo "${BLD}=========================================${RST}"

# ------------------------------------------------------------------
step "1/7  檢查環境"
# ------------------------------------------------------------------
[ "$(id -u)" -eq 0 ] && die "不要用 root 跑。用一般使用者，需要時它會自己叫 sudo。"
[ "$(uname -m)" = "x86_64" ] || die "需要 x86_64（要編 X64 的 EFI）。你現在是 $(uname -m)。"

. /etc/os-release 2>/dev/null || true
echo "  發行版：${PRETTY_NAME:-未知}"

IS_WSL=0
if grep -qi microsoft /proc/version 2>/dev/null; then
    IS_WSL=1
    echo "  環境  ：WSL"
else
    echo "  環境  ：$(systemd-detect-virt 2>/dev/null || echo '實體機')"
fi

case "${VERSION_ID:-}" in
    22.04) ok "Ubuntu 22.04，這是建議版本" ;;
    24.04|24.10|25.*|26.*)
        warn "Ubuntu ${VERSION_ID} 的 Python 是 3.12+，已移除 distutils"
        warn "edk2 BaseTools 可能會編不過。編不過的話請改用 22.04。"
        ;;
    20.04) warn "Ubuntu 20.04 有點舊但通常可以" ;;
    *)     warn "沒測過 ${PRETTY_NAME:-這個發行版}，繼續試" ;;
esac

avail_gb=$(( $(df -Pk "$HOME" | awk 'NR==2{print $4}') / 1024 / 1024 ))
if [ "$avail_gb" -lt 15 ]; then
    die "$HOME 只剩 ${avail_gb} GB。edk2 + submodule + Build 目錄至少要 15 GB。"
fi
ok "磁碟空間 ${avail_gb} GB 夠用"

sudo -v || die "需要 sudo 權限才能裝套件。"

# ------------------------------------------------------------------
step "2/7  安裝相依套件"
# ------------------------------------------------------------------
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential uuid-dev acpica-tools git nasm \
    python3 python3-dev python3-pip python-is-python3 \
    || die "apt 安裝失敗。先跑 sudo apt-get update 看看網路通不通。"

# BaseTools 有些版本會 import distutils。Python 3.12 起標準庫已移除它，
# 22.04（Python 3.10）裝得到，24.04 裝不到 —— 裝不到就算了，讓它往下走。
if apt-cache show python3-distutils >/dev/null 2>&1; then
    sudo apt-get install -y python3-distutils && ok "python3-distutils 已裝"
else
    warn "這個 Ubuntu 沒有 python3-distutils（Python 3.12+ 已移除）"
fi
ok "套件安裝完成"
echo "    gcc     : $(gcc -dumpversion)"
echo "    python3 : $(python3 --version 2>&1 | awk '{print $2}')"
echo "    nasm    : $(nasm -v | awk '{print $3}')"

# ------------------------------------------------------------------
step "3/7  取得 edk2 原始碼（$EDK2_TAG）"
# ------------------------------------------------------------------
if [ -d "$EDK2_DIR/.git" ]; then
    ok "$EDK2_DIR 已存在，跳過 clone"
else
    echo "  clone 中（約 300 MB，看網速可能要幾分鐘）..."
    git clone --depth 1 --branch "$EDK2_TAG" "$EDK2_REPO" "$EDK2_DIR" 2>/dev/null || {
        warn "抓不到 tag $EDK2_TAG，改用完整 clone"
        rm -rf "$EDK2_DIR"
        git clone "$EDK2_REPO" "$EDK2_DIR" || die "git clone 失敗，檢查網路。"
    }
fi

cd "$EDK2_DIR"
if ! git describe --tags --exact-match HEAD >/dev/null 2>&1; then
    git fetch --tags --depth 1 --quiet 2>/dev/null || true
    if git rev-parse --verify --quiet "refs/tags/$EDK2_TAG" >/dev/null; then
        git checkout --quiet "$EDK2_TAG"
    else
        FALLBACK="$(git tag -l 'edk2-stable*' | sort -V | tail -1)"
        [ -n "$FALLBACK" ] || die "找不到任何 edk2-stable tag。"
        warn "$EDK2_TAG 找不到，改用 $FALLBACK"
        git checkout --quiet "$FALLBACK"
    fi
fi
ok "edk2 版本：$(git describe --tags 2>/dev/null || git rev-parse --short HEAD)"

# ------------------------------------------------------------------
step "4/7  取得 submodule"
# ------------------------------------------------------------------
# ★ 漏了這步，BaseTools 會噴一堆找不到標頭檔 ★
#
# ★★ 這裡不要加 --depth 1 ★★
#
# --depth 1 只抓每個 submodule「預設分支的最新一筆」。但 edk2 是把 submodule
# 釘在特定 commit 上的，那一筆通常不是最新的。抓不到就會噴：
#     error: Server does not allow request for unadvertised object <sha>
#     fatal: reference is not a tree: <sha>
#
# 陰險的地方：它是「機率性」的。被釘的 commit 剛好還是分支頂端時就會成功，
# 上游一推新東西就開始失敗 —— 所以會出現「同一份腳本 A 電腦好好的、B 電腦爆」。
# 完整抓多花幾分鐘、多約 200 MB，但不會看運氣。實際踩過才改成這樣。
#
# ★★ 而且不要無條件 --recursive ★★
#
# edk2 有十幾個 submodule，但編 UefiApplication（只用 MdePkg）真正需要的
# 只有 BaseTools 的 brotli 一個。其餘都是別的 package 在用的：
#     openssl    -> CryptoPkg
#     libspdm    -> SecurityPkg    <- 它自己底下還有一層 submodule
#     googletest -> UnitTestFrameworkPkg
#     jansson    -> RedfishPkg
#
# --recursive 會遞迴進 libspdm 去抓「它的」submodule，那層失敗就整個 die：
#     fatal: Failed to recurse into submodule path 'SecurityPkg/.../libspdm'
# 於是一個你根本用不到的 package，擋住了整個安裝。實際踩過。
#
# 所以拆成兩段：brotli 是必要的（失敗就 die），其餘是加分的（失敗只警告）。

# --- 必要：BaseTools 需要 brotli，沒有它 BaseTools 編不過 ---
if ! git submodule update --init BaseTools/Source/C/BrotliCompress/brotli; then
    # URL 變過的話 .git/config 會留著舊的，症狀跟抓不到一樣，sync 一下再試
    warn "brotli 抓取失敗，對齊 URL 後重試..."
    git submodule sync --recursive >/dev/null 2>&1 || true
    git submodule update --init BaseTools/Source/C/BrotliCompress/brotli || die "brotli 抓取失敗。
       這個是必要的（BaseTools 需要）。跑這支診斷：
           $SCRIPT_DIR/fix-submodule.sh --diag"
fi
ok "brotli 已取得（BaseTools 需要的唯一一個）"

# --- 選用：其餘 submodule，抓不到也不該擋住你 ---
echo "  嘗試抓取其餘 submodule（用不到，但抓了無妨）..."
if git submodule update --init --recursive >/dev/null 2>&1; then
    ok "其餘 submodule 也都抓好了"
else
    warn "部分 submodule 沒抓到（常見：libspdm / openssl）"
    warn "編 UefiApplication 用不到它們，${BLD}繼續往下${RST}。"
    warn "真的需要的話（例如要編 CryptoPkg）再跑：$SCRIPT_DIR/fix-submodule.sh"
fi

# ------------------------------------------------------------------
step "5/7  編譯 BaseTools"
# ------------------------------------------------------------------
# ★★ 這是最多人卡住的一步 ★★
# edk2 的 `build` 不是 apt 裝得到的指令。它是 BaseTools 編出來的 Python 工具，
# 再由 edksetup.sh 匯出到 PATH 才會出現。少了這步就會一直
# 「build: command not found」—— 你之前八成就是卡在這。
if [ -x "$EDK2_DIR/BaseTools/Source/C/bin/GenFw" ]; then
    ok "BaseTools 已經編過了，跳過"
else
    echo "  編譯中（約 1-3 分鐘）..."
    make -C "$EDK2_DIR/BaseTools" -j"$(nproc)" || die "BaseTools 編譯失敗。把上面的錯誤訊息給我。"
    ok "BaseTools 編譯完成"
fi

# ------------------------------------------------------------------
step "6/7  安裝 ocbuild 指令並設定路徑"
# ------------------------------------------------------------------
# 你說的「路徑之類的」就是這段。
#
# edk2 官方的做法是每開一個新終端機都要手動：
#     cd ~/edk2 && source edksetup.sh
# 忘了 source、或用 ./edksetup.sh 直接執行（環境變數只留在子行程，
# 一結束就沒了），`build` 就again不見。這是第二大坑。
#
# 所以這裡裝一個 ocbuild 指令：它自己處理 WORKSPACE / PACKAGES_PATH /
# source edksetup.sh，你在任何目錄、任何終端機打 ocbuild 就能編。
mkdir -p "$BIN_DIR"
cat > "$BIN_DIR/ocbuild" <<OCBUILD_EOF
#!/usr/bin/env bash
# 由 setup-edk2-ubuntu.sh 自動產生。直接改這個檔沒關係，重跑 setup 會蓋掉。
exec "$SCRIPT_DIR/build-efi.sh" "\$@"
OCBUILD_EOF
chmod +x "$BIN_DIR/ocbuild"
ok "已安裝 $BIN_DIR/ocbuild"

# 把 ~/.local/bin 加進 PATH（只加一次，重跑不會重複）
MARKER="# >>> edk2 / UsbOcRecover <<<"
if ! grep -qF "$MARKER" "$HOME/.bashrc" 2>/dev/null; then
    cat >> "$HOME/.bashrc" <<BASHRC_EOF

$MARKER
export PATH="\$HOME/.local/bin:\$PATH"
export EDK2_DIR="$EDK2_DIR"
# 想手動用 edk2 官方流程的話，打 edk2env 就會設好環境（一定要用 source 的原因見 README）
edk2env() { cd "$EDK2_DIR" && source ./edksetup.sh && echo "edk2 環境已就緒，可以用 build 指令了"; }
$MARKER
BASHRC_EOF
    ok "已把 ~/.local/bin 加進 ~/.bashrc 的 PATH"
else
    ok "~/.bashrc 已經設定過了，不重複加"
fi
export PATH="$BIN_DIR:$PATH"

# ------------------------------------------------------------------
step "7/7  驗證：實際編一次 UsbOcRecover.efi"
# ------------------------------------------------------------------
# 兩種擺法都支援：腳本跟 package 同層（git repo）或在子資料夾裡（OCV2 專案）
if [ ! -f "$SCRIPT_DIR/UsbOcRecoverPkg/UsbOcRecoverPkg.dsc" ] &&
   [ ! -f "$SCRIPT_DIR/../UsbOcRecoverPkg/UsbOcRecoverPkg.dsc" ]; then
    warn "找不到 UsbOcRecoverPkg，跳過驗證。"
    warn "環境本身已就緒，把專案放好之後跑 ocbuild 即可。"
else
    "$SCRIPT_DIR/build-efi.sh" || die "驗證編譯失敗。把上面的錯誤訊息給我。"
fi

# ------------------------------------------------------------------
echo
echo "${GRN}${BLD}================ 安裝完成 ================${RST}"
echo "  edk2      : $EDK2_DIR"
echo "  版本      : $(git -C "$EDK2_DIR" describe --tags 2>/dev/null)"
echo "  編譯指令  : ${BLD}ocbuild${RST}          （任何目錄都能用）"
echo "  診斷指令  : ${BLD}./setup-edk2-ubuntu.sh --check${RST}"
echo
echo "  ${YLW}這個終端機要先跑一次這行，ocbuild 才會生效：${RST}"
echo "      ${BLD}source ~/.bashrc${RST}"
echo "  （之後新開的終端機就不用了，會自動載入）"
echo "${GRN}${BLD}==========================================${RST}"
