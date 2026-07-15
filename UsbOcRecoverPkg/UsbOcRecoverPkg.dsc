## @file
#  UsbOcRecoverPkg.dsc - Standalone build DSC for the UsbOcRecover UEFI application.
#
#  NOTE: Keep this file pure ASCII. edk2's build.py reads DSC/INF with the
#  locale's preferred encoding, which is cp950 on a Traditional Chinese
#  Windows. Any UTF-8 non-ASCII byte here fails to decode and build aborts
#  with the unhelpful "error 0004: File read failure". Chinese belongs in
#  the .md docs and .c comments, never in DSC/INF.
#
#  Linux/GCC build (scripts live in the ubuntu build-tools folder):
#    build -a X64 -t GCC5 -p UsbOcRecoverPkg/UsbOcRecoverPkg.dsc -b RELEASE
#  Windows/VS2022 build:
#    build -a X64 -t VS2022 -p UsbOcRecoverPkg/UsbOcRecoverPkg.dsc -b RELEASE
#  Output:
#    Build/UsbOcRecoverPkg/RELEASE_<toolchain>/X64/UsbOcRecover.efi
#
#  Copyright (c) 2025-2026, UsbOcRecover Contributors. All rights reserved.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = UsbOcRecoverPkg
  PLATFORM_GUID                  = B1C2D3E4-F5A6-7890-ABCD-EF1234567890
  PLATFORM_VERSION               = 2.0
  DSC_SPECIFICATION              = 0x00010006
  OUTPUT_DIRECTORY               = Build/UsbOcRecoverPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

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
  UefiDevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xFF
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000040

[Components]
  UsbOcRecoverPkg/UsbOcRecover/UsbOcRecover.inf
