#!/usr/bin/env bash
#
# new-efi-project.sh —— 產生一個可以馬上編譯的空白 EFI 專案
#
# 用法：
#   ./new-efi-project.sh MyApp          在 ~/MyApp 建立專案
#   ./new-efi-project.sh MyApp ~/code   指定放哪
#
# 建好之後：
#   cd ~/MyApp
#   ./build.sh          <- 編譯
#   改 MyApp/MyApp.c 然後再 ./build.sh
#
set -euo pipefail

NAME="${1:-}"
DEST_ROOT="${2:-$HOME}"

RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; BLD=$'\033[1m'; RST=$'\033[0m'
die() { echo "${RED}[失敗] $*${RST}" >&2; exit 1; }

[ -n "$NAME" ] || die "要給專案名字。例如：./new-efi-project.sh MyApp"
# 名字會直接變成 C 的識別字與檔名，限制成 ASCII 開頭字母
echo "$NAME" | grep -qE '^[A-Za-z][A-Za-z0-9]*$' \
    || die "名字只能用英文字母和數字，且開頭是字母（會用在 .inf 和函式名）。"

PROJ="$DEST_ROOT/$NAME"
[ -e "$PROJ" ] && die "$PROJ 已經存在了。換個名字或先砍掉。"

# 每個專案都要有自己的 GUID，抄別人的會衝突
GUID_MOD=$(cat /proc/sys/kernel/random/uuid)
GUID_PKG=$(cat /proc/sys/kernel/random/uuid)

mkdir -p "$PROJ/${NAME}Pkg/$NAME"

# ------------------------------------------------------------------
#  1. 程式碼
# ------------------------------------------------------------------
# 注意：這個範本刻意不含中文。要加中文註解的話，檔案必須存成
# 「UTF-8 with BOM」，否則用 MSVC 編會爆（GCC 則無所謂）。
cat > "$PROJ/${NAME}Pkg/$NAME/$NAME.c" <<EOF
/** @file
  $NAME.c - my UEFI application

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS
EFIAPI
${NAME}Main (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN          Index;
  EFI_INPUT_KEY  Key;

  Print (L"\r\n");
  Print (L"==============================\r\n");
  Print (L"  $NAME\r\n");
  Print (L"==============================\r\n");
  Print (L"\r\n");
  Print (L"UEFI Version : %d.%d\r\n",
         SystemTable->Hdr.Revision >> 16,
         SystemTable->Hdr.Revision & 0xFFFF);
  Print (L"Firmware     : %s\r\n", SystemTable->FirmwareVendor);

  Print (L"\r\nPress any key to exit...\r\n");
  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);

  return EFI_SUCCESS;
}
EOF

# ------------------------------------------------------------------
#  2. INF —— 描述「這一個模組」
# ------------------------------------------------------------------
# ★ INF / DSC 一律純 ASCII，不要寫中文 ★
# edk2 的 build.py 用系統偏好編碼讀它們，非 UTF-8 的 locale（例如繁中
# Windows 的 cp950）會解碼失敗，只給你 "error 0004: File read failure"。
cat > "$PROJ/${NAME}Pkg/$NAME/$NAME.inf" <<EOF
## @file
#  $NAME - UEFI Shell Application (X64)
#
#  Keep this file pure ASCII. See new-efi-project.sh for why.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  INF_VERSION    = 0x00010006
  BASE_NAME      = $NAME
  FILE_GUID      = $GUID_MOD
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  ENTRY_POINT    = ${NAME}Main

[Sources]
  $NAME.c

[Packages]
  MdePkg/MdePkg.dec

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib
  UefiBootServicesTableLib

# 需要更多功能就在這裡加，例如：
# [Protocols]
#   gEfiPciIoProtocolGuid                 ## CONSUMES
#   gEfiSimpleFileSystemProtocolGuid      ## CONSUMES
EOF

# ------------------------------------------------------------------
#  3. DSC —— 描述「整包怎麼組起來」
# ------------------------------------------------------------------
cat > "$PROJ/${NAME}Pkg/${NAME}Pkg.dsc" <<EOF
## @file
#  ${NAME}Pkg.dsc - standalone build DSC
#
#  Keep this file pure ASCII.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME           = ${NAME}Pkg
  PLATFORM_GUID           = $GUID_PKG
  PLATFORM_VERSION        = 1.0
  DSC_SPECIFICATION       = 0x00010006
  OUTPUT_DIRECTORY        = Build/${NAME}Pkg
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS           = DEBUG|RELEASE
  SKUID_IDENTIFIER        = DEFAULT

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xFF
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000040

[Components]
  ${NAME}Pkg/$NAME/$NAME.inf
EOF

# ------------------------------------------------------------------
#  4. 這個專案自己的 build.sh
# ------------------------------------------------------------------
cat > "$PROJ/build.sh" <<'BUILDEOF'
#!/usr/bin/env bash
#
# 編譯這個專案。用法： ./build.sh  或  ./build.sh DEBUG
#
set -uo pipefail

TARGET="${1:-RELEASE}"
EDK2_DIR="${EDK2_DIR:-$HOME/edk2}"
TOOLCHAIN="${TOOLCHAIN:-GCC5}"
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME="__NAME__"

GRN=$'\033[32m'; RED=$'\033[31m'; BLD=$'\033[1m'; RST=$'\033[0m'
die() { echo "${RED}${BLD}[失敗] $*${RST}" >&2; exit 1; }

case "$TARGET" in RELEASE|DEBUG) ;; *) die "只能是 RELEASE 或 DEBUG" ;; esac
[ -d "$EDK2_DIR" ] || die "找不到 edk2（$EDK2_DIR）。先跑 EFIVMV1/setup-edk2-ubuntu.sh"

set -e
cd "$EDK2_DIR"
set +u
# shellcheck disable=SC1091
source ./edksetup.sh >/dev/null || die "source edksetup.sh 失敗"
set -u

#
# ★ 這三行是重點，不要簡化 ★
#
# WORKSPACE 一定要指到「你的專案」而不是 edk2。
# `build -p XxxPkg/Xxx.dsc` 是相對路徑，edk2 先用 WORKSPACE 解析，
# PACKAGES_PATH 只是後備。如果 WORKSPACE=edk2 而 edk2 底下剛好有同名
# package，build 會安靜地編那份舊的、回報成功，你完全不會發現。
#
export WORKSPACE="$PROJ_DIR"
export PACKAGES_PATH="$PROJ_DIR:$EDK2_DIR"
export CONF_PATH="$EDK2_DIR/Conf"

echo "${BLD}==> 編譯 $NAME ($TARGET)${RST}"
build -a X64 -t "$TOOLCHAIN" -p "${NAME}Pkg/${NAME}Pkg.dsc" -b "$TARGET" \
    || die "編譯失敗，看上面的錯誤訊息。"

EFI="$PROJ_DIR/Build/${NAME}Pkg/${TARGET}_${TOOLCHAIN}/X64/${NAME}.efi"
[ -f "$EFI" ] || die "build 說成功了，但找不到 $EFI"
cp "$EFI" "$PROJ_DIR/${NAME}.efi"

echo
echo "${GRN}${BLD}================ 編譯成功 ================${RST}"
echo "  ${BLD}$PROJ_DIR/${NAME}.efi${RST}"
echo "  大小 : $(stat -c %s "$PROJ_DIR/${NAME}.efi") bytes"
echo "  類型 : $(file -b "$PROJ_DIR/${NAME}.efi" 2>/dev/null)"
echo
echo "  放到 USB（FAT32）的 EFI/BOOT/BOOTX64.EFI 就能開機執行"
echo "${GRN}${BLD}==========================================${RST}"
BUILDEOF
sed -i "s/__NAME__/$NAME/" "$PROJ/build.sh"
chmod +x "$PROJ/build.sh"

cat > "$PROJ/.gitignore" <<'EOF'
Build/
*.efi
EOF

# ------------------------------------------------------------------
echo
echo "${GRN}${BLD}==> 專案建好了：$PROJ${RST}"
echo
echo "  ${NAME}Pkg/$NAME/$NAME.c     <- 改這個（你的程式碼）"
echo "  ${NAME}Pkg/$NAME/$NAME.inf   <- 加檔案 / 加 Protocol 時改這個"
echo "  ${NAME}Pkg/${NAME}Pkg.dsc    <- 加函式庫時改這個"
echo "  build.sh                     <- 編譯"
echo
echo "${BLD}現在就試：${RST}"
echo "    cd $PROJ"
echo "    ./build.sh"
echo
echo "${YLW}提醒：.inf 和 .dsc 不要寫中文（會噴 File read failure）。${RST}"
echo "${YLW}      .c 要寫中文的話，存檔要選 UTF-8 with BOM。${RST}"
