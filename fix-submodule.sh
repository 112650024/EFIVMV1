#!/usr/bin/env bash
#
# fix-submodule.sh —— 診斷並修復 edk2 submodule 抓取失敗
#
# ============================================================
#  為什麼會失敗（先看懂再修）
# ============================================================
#
#   舊版 setup-edk2-ubuntu.sh 用了：
#       git submodule update --init --recursive --depth 1
#                                              ^^^^^^^^^ 問題在這
#
#   --depth 1 只抓每個 submodule「預設分支的最新一筆」。
#   但 edk2 是把 submodule 釘在**特定 commit** 上的，那一筆通常不是最新的。
#   抓不到被釘的那個 commit，就會噴：
#
#       error: Server does not allow request for unadvertised object <sha>
#       fatal: reference is not a tree: <sha>
#       Unable to checkout '<sha>' in submodule path '...'
#
#   ★ 關鍵：它是「機率性」的 ★
#   看你 clone 的當下上游有沒有推新 commit。如果被釘的剛好還是分支頂端，
#   --depth 1 就會成功。所以會出現「同一份腳本，A 電腦好好的、B 電腦就爆」，
#   甚至「昨天可以、今天不行」。這不是你做錯什麼。
#
#   解法就是不要用 --depth 1。完整抓會多花幾分鐘、多約 200 MB，
#   但不會看運氣。
#
# ============================================================
#  用法
# ============================================================
#   ./fix-submodule.sh            診斷 + 完整修復（最保險，預設）
#   ./fix-submodule.sh --diag     只診斷，完全不動你的東西
#   ./fix-submodule.sh --minimal  只抓 BaseTools 真正需要的（快很多）
#   ./fix-submodule.sh --help     說明
#
# 環境變數：
#   EDK2_DIR=$HOME/edk2           edk2 在哪
#
set -uo pipefail

EDK2_DIR="${EDK2_DIR:-$HOME/edk2}"

RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; CYN=$'\033[36m'; BLD=$'\033[1m'; RST=$'\033[0m'
step()  { echo; echo "${BLD}${CYN}==> $*${RST}"; }
ok()    { echo "  ${GRN}[OK]${RST}   $*"; }
warn()  { echo "  ${YLW}[注意]${RST} $*"; }
fail()  { echo "  ${RED}[X]${RST}    $*"; }
die()   { echo; echo "${RED}${BLD}[失敗] $*${RST}" >&2; exit 1; }

MODE="fix"
case "${1:-}" in
    --diag)     MODE="diag" ;;
    --minimal)  MODE="minimal" ;;
    --help|-h)  sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "")         ;;
    *)          die "不認得的參數：$1（用 --help 看說明）" ;;
esac

#
# BaseTools 真正需要的 submodule。
#
# 你的用途是編 UefiApplication（只用 MdePkg），BaseTools 的 C 工具裡只有
# BrotliCompress 需要 submodule。CryptoPkg 的 openssl（最大、最常抓失敗的
# 那個）你根本用不到。--minimal 就是只抓這些。
#
MINIMAL_PATHS=(
    "BaseTools/Source/C/BrotliCompress/brotli"
)

echo "${BLD}=========================================${RST}"
echo "${BLD}  edk2 submodule 診斷 / 修復${RST}"
echo "${BLD}=========================================${RST}"

# ==================================================================
step "1. 基本檢查"
# ==================================================================
[ -d "$EDK2_DIR" ]      || die "找不到 $EDK2_DIR。先跑 ./setup-edk2-ubuntu.sh。"
[ -d "$EDK2_DIR/.git" ] || die "$EDK2_DIR 不是 git repo。整個砍掉重跑 setup 比較快：
       rm -rf $EDK2_DIR && ./setup-edk2-ubuntu.sh"
command -v git >/dev/null 2>&1 || die "git 沒裝：sudo apt install -y git"

cd "$EDK2_DIR"
ok "edk2 位置：$EDK2_DIR"
ok "edk2 版本：$(git describe --tags 2>/dev/null || git rev-parse --short HEAD)"

# ==================================================================
step "2. 診斷"
# ==================================================================
PROBLEMS=0

# ---- 2a. 父 repo 是不是淺的 ----
IS_SHALLOW="$(git rev-parse --is-shallow-repository 2>/dev/null || echo unknown)"
if [ "$IS_SHALLOW" = "true" ]; then
    warn "父 repo 是 shallow clone（--depth 1）"
    echo "         這本身不是錯，但配上 --depth 1 的 submodule 就容易出事。"
else
    ok "父 repo 是完整 clone"
fi

# ---- 2b. 磁碟空間 ----
AVAIL_GB=$(( $(df -Pk "$EDK2_DIR" | awk 'NR==2{print $4}') / 1024 / 1024 ))
if [ "$AVAIL_GB" -ge 3 ]; then
    ok "磁碟還有 ${AVAIL_GB} GB"
else
    fail "磁碟只剩 ${AVAIL_GB} GB —— submodule 完整抓約要 2 GB"
    echo "         磁碟不足會讓 git 在抓到一半時失敗，錯誤訊息看起來像網路問題。"
    PROBLEMS=$((PROBLEMS+1))
fi

# ---- 2c. 連得到 GitHub 嗎 ----
if git ls-remote --exit-code https://github.com/tianocore/edk2.git HEAD >/dev/null 2>&1; then
    ok "連得到 GitHub"
else
    fail "連不到 GitHub"
    echo "         submodule 抓取失敗最常見的第二個原因就是網路 / proxy。"
    echo "         VM 的話檢查網路設定是不是 NAT。"
    PROBLEMS=$((PROBLEMS+1))
fi

# ---- 2d. 哪些 submodule 沒抓到 ----
#
# git submodule status 的第一個字元：
#   '-' = 沒 init（就是你現在的狀況）
#   '+' = checkout 到不同的 commit
#   ' ' = 正常
#
echo
echo "  ${BLD}submodule 狀態：${RST}"
MISSING=0
TOTAL=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    TOTAL=$((TOTAL+1))
    flag="${line:0:1}"
    path="$(echo "$line" | awk '{print $2}')"
    case "$flag" in
        '-') echo "    ${RED}[沒抓]${RST}  $path"; MISSING=$((MISSING+1)) ;;
        '+') echo "    ${YLW}[版本不符]${RST} $path" ;;
        *)   echo "    ${GRN}[OK]${RST}    $path" ;;
    esac
done < <(git submodule status 2>/dev/null)

echo
#
# ★ 「有幾個沒抓到」本身不是問題，不要拿它當判斷標準 ★
#
# edk2 十幾個 submodule 裡，編 UefiApplication 只需要 brotli 一個。
# 拿「全部抓到了沒」當標準會誤報 —— 你明明可以編，它卻說你有問題。
# 真正的判斷標準只有下面那個 brotli 檢查。
#
if [ "$TOTAL" -eq 0 ]; then
    warn "讀不到 submodule 清單（.gitmodules 可能不見了）"
    PROBLEMS=$((PROBLEMS+1))
elif [ "$MISSING" -gt 0 ]; then
    echo "  ${YLW}[資訊]${RST} $TOTAL 個 submodule 裡有 $MISSING 個沒抓到"
    echo "         先別緊張 —— 重點是 brotli 在不在，看下一項。"
else
    ok "$TOTAL 個 submodule 都在"
fi

# ---- 2e. BaseTools 能不能編（真正的重點） ----
BROTLI="$EDK2_DIR/BaseTools/Source/C/BrotliCompress/brotli"
if [ -n "$(ls -A "$BROTLI" 2>/dev/null)" ]; then
    ok "brotli 有抓到 ${BLD}<- BaseTools 只需要這個${RST}"
else
    fail "brotli 沒抓到 —— ${BLD}BaseTools 會編不過${RST}"
    echo "         這是唯一會擋住你的 submodule。"
    PROBLEMS=$((PROBLEMS+1))
fi

OPENSSL="$EDK2_DIR/CryptoPkg/Library/OpensslLib/openssl"
if [ -n "$(ls -A "$OPENSSL" 2>/dev/null)" ]; then
    ok "openssl 有抓到（你用不到，但抓了也無妨）"
else
    warn "openssl 沒抓到 —— ${BLD}但編 UefiApplication 用不到它${RST}"
    echo "         它是最大、最常抓失敗的那個。你只用 MdePkg，不需要它。"
fi

# ==================================================================
if [ "$MODE" = "diag" ]; then
    echo
    if [ "$PROBLEMS" -eq 0 ]; then
        echo "${GRN}${BLD}==> 沒發現問題${RST}"
        exit 0
    fi
    echo "${YLW}${BLD}==> 發現 $PROBLEMS 個問題。跑 ./fix-submodule.sh（不加參數）可以修。${RST}"
    echo "    只想快點編起來的話：./fix-submodule.sh --minimal"
    exit 1
fi

# ==================================================================
step "3. 修復"
# ==================================================================

# ---- 3a. 先對齊 URL ----
# .gitmodules 裡的網址如果變過（上游搬家），舊的 .git/config 會留著舊網址，
# 症狀跟抓不到一模一樣。sync 會把它對齊回來。
echo "  對齊 submodule URL..."
git submodule sync --recursive >/dev/null 2>&1 || warn "sync 失敗，繼續試"

if [ "$MODE" = "minimal" ]; then
    # ------------------------------------------------------------------
    #  最小修復：只抓 BaseTools 需要的
    # ------------------------------------------------------------------
    echo
    echo "  ${BLD}最小模式：只抓 BaseTools 真正需要的 submodule${RST}"
    echo "  （跳過 openssl 等你用不到的，快很多）"
    echo

    for p in "${MINIMAL_PATHS[@]}"; do
        echo "  抓取 $p ..."
        if git submodule update --init "$p"; then
            ok "$p"
        else
            die "$p 抓取失敗。

       這個是必要的，不能跳過。試試完整修復：
           ./fix-submodule.sh

       還是不行的話，把上面的錯誤訊息貼給我。"
        fi
    done
else
    # ------------------------------------------------------------------
    #  完整修復：策略階梯，一層一層往下試
    # ------------------------------------------------------------------
    echo
    echo "  ${BLD}完整模式：抓所有 submodule（約 200 MB，幾分鐘）${RST}"
    echo "  ${BLD}關鍵：不加 --depth 1${RST}"
    echo

    FIXED=0

    # 策略 A：正常的完整抓取（九成的情況這樣就好了）
    echo "  [策略 1/3] git submodule update --init --recursive"
    if git submodule update --init --recursive; then
        ok "成功"
        FIXED=1
    else
        warn "策略 1 失敗，往下試"
    fi

    # 策略 B：父 repo 是淺的 -> 拉成完整的再試
    #
    # 父 repo 淺的時候，git 有時候算不出 submodule 該用哪個 commit。
    # unshallow 會把完整歷史拉下來（edk2 約 +500 MB），然後就能正確解析。
    if [ "$FIXED" -eq 0 ] && [ "$IS_SHALLOW" = "true" ]; then
        echo
        echo "  [策略 2/3] 父 repo 是 shallow，先拉成完整的再試"
        echo "            （會多下載約 500 MB，這是為了讓 git 能正確解析 submodule commit）"
        if git fetch --unshallow 2>/dev/null || git fetch --depth=1000000 2>/dev/null; then
            ok "父 repo 已拉成完整"
            if git submodule update --init --recursive; then
                ok "成功"
                FIXED=1
            else
                warn "策略 2 還是失敗，往下試"
            fi
        else
            warn "unshallow 失敗，往下試"
        fi
    fi

    # 策略 C：逐個 deinit 重來
    #
    # 前面失敗常常是因為 .git/modules/ 底下留了半殘的狀態。
    # deinit -f 會把它清掉重新開始。
    if [ "$FIXED" -eq 0 ]; then
        echo
        echo "  [策略 3/3] 清掉半殘狀態，逐個重抓"
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            flag="${line:0:1}"
            path="$(echo "$line" | awk '{print $2}')"
            [ "$flag" = " " ] && continue
            echo "    重抓 $path ..."
            git submodule deinit -f "$path" >/dev/null 2>&1 || true
            rm -rf "$EDK2_DIR/.git/modules/$path"
            git submodule update --init "$path" >/dev/null 2>&1 \
                && echo "      ${GRN}OK${RST}" \
                || echo "      ${RED}還是失敗${RST}"
        done < <(git submodule status 2>/dev/null)
    fi
fi

# ==================================================================
step "4. 驗證"
# ==================================================================

# 重新確認 brotli —— 這才是會不會擋住你的關鍵
if [ -n "$(ls -A "$BROTLI" 2>/dev/null)" ]; then
    ok "brotli 在 —— BaseTools 可以編了"
else
    die "brotli 還是沒抓到，BaseTools 會編不過。

       手動試這行，把完整錯誤訊息貼給我：
           git -C $EDK2_DIR submodule update --init BaseTools/Source/C/BrotliCompress/brotli"
fi

STILL_MISSING=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    [ "${line:0:1}" = "-" ] && STILL_MISSING=$((STILL_MISSING+1))
done < <(git submodule status 2>/dev/null)

if [ "$STILL_MISSING" -gt 0 ]; then
    warn "還有 $STILL_MISSING 個 submodule 沒抓到"
    echo "         如果 brotli 在（上面是 OK），${BLD}這不會擋住你編 UefiApplication${RST}。"
    echo
    echo "         沒抓到的通常是這些，它們屬於你用不到的 package："
    echo "           openssl    -> CryptoPkg"
    echo "           libspdm    -> SecurityPkg（它自己底下還有一層 submodule，"
    echo "                         所以 --recursive 最常炸在這個）"
    echo "           googletest -> UnitTestFrameworkPkg"
    echo "           jansson    -> RedfishPkg"
    echo
    echo "         你編的是 UefiApplication，只用 MdePkg。${BLD}可以直接往下跑 setup。${RST}"
else
    ok "所有 submodule 都在"
fi

# ==================================================================
echo
echo "${GRN}${BLD}================ 修復完成 ================${RST}"
echo
echo "  接下來："
echo "      ${BLD}cd $(dirname "${BASH_SOURCE[0]}")${RST}"
echo "      ${BLD}./setup-edk2-ubuntu.sh${RST}      # 可以重複跑，抓好的會自動跳過"
echo
echo "  它會從「編譯 BaseTools」那步繼續往下。"
echo "${GRN}${BLD}==========================================${RST}"
