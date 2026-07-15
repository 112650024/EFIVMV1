/** @file
  UsbOcRecover.c - xHCI Over-Current (OC) 偵測與復原工具  v2.0

  【v2.0 相對 v1.1 的變更】
  1. 修正 PORTSC 讀-改-寫會誤清 PED 的嚴重 bug（見下方 PORTSC_PRESERVE 說明）
  2. 主選單精簡為 4 項：[1] 掃描 [5] 一鍵復原 [9] 詳細診斷 [0] 離開
  3. [9] 詳細診斷改為完整說明：位址怎麼算、USB2/3 怎麼判斷、
     這個 port 對應到哪個 fs、以及「這個 port 到底發生什麼事」
  4. 復原流程新增 PLS 導向步驟：reset 後若 PLS 仍卡在 Disabled，
     以 LWS 寫入 PLS=RxDetect 把 link 拉回來

  【安全警告】
  本工具直接寫入 xHCI MMIO 暫存器，會影響 USB 電源狀態。
  OCA==1（過流來源還在）時，本工具一律拒絕上電與 reset。

  Copyright (c) 2025-2026, UsbOcRecover Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>
#include <IndustryStandard/Pci.h>

// ============================================================
//  版本資訊
// ============================================================
#define USBOCREC_VERSION_STR      L"2.0"

// ============================================================
//  PCI Class Code / BAR
// ============================================================
#define PCI_CLASS_SERIAL          0x0C
#define PCI_SUBCLASS_USB          0x03
#define PCI_IF_XHCI               0x30

#define PCI_BAR0_OFFSET           0x10
#define PCI_BAR1_OFFSET           0x14
#define PCI_BAR_MEMORY_TYPE_MASK  0x06
#define PCI_BAR_MEMORY_TYPE_64    0x04

// ============================================================
//  xHCI Capability Registers（相對 MMIO_BASE）- xHCI Spec 5.3
// ============================================================
#define XHCI_CAPLENGTH_OFFSET     0x00   // 1-byte
#define XHCI_HCIVERSION_OFFSET    0x02   // 2-byte
#define XHCI_HCSPARAMS1_OFFSET    0x04   // 4-byte
#define XHCI_HCCPARAMS1_OFFSET    0x10   // 4-byte (xECP pointer 在 bit 31:16)

#define HCSPARAMS1_MAXPORTS_SHIFT 24
#define HCSPARAMS1_MAXPORTS_MASK  0xFF000000UL
#define HCCPARAMS1_XECP_SHIFT     16

#define XHCI_ECAP_ID_SUPPORTED_PROTOCOL  0x02

// ============================================================
//  xHCI Operational Registers（相對 OperationalBase）- xHCI Spec 5.4
// ============================================================
#define XHCI_USBCMD_OFFSET        0x00
#define XHCI_USBSTS_OFFSET        0x04
#define XHCI_PORTSC_BASE_OFFSET   0x400
#define XHCI_PORTSC_PORT_STRIDE   0x10

#define USBCMD_RS                 BIT0   // Run/Stop
#define USBCMD_HCRST              BIT1   // Host Controller Reset
#define USBSTS_HCH                BIT0   // HCHalted
#define USBSTS_CNR                BIT11  // Controller Not Ready

// ============================================================
//  PORTSC Bit 定義（xHCI Spec Table 5-27）
// ============================================================
#define PORTSC_CCS        BIT0    // Current Connect Status        (ROS)
#define PORTSC_PED        BIT1    // Port Enabled/Disabled         (RW1CS!)
#define PORTSC_OCA        BIT3    // Over-Current Active           (RO)  <- 關鍵安全 bit
#define PORTSC_PR         BIT4    // Port Reset                    (RW1S)
#define PORTSC_PLS_SHIFT  5
#define PORTSC_PLS_MASK   (BIT5|BIT6|BIT7|BIT8)   // Port Link State (RWS)
#define PORTSC_PP         BIT9    // Port Power                    (RWS)
#define PORTSC_SPEED_MASK (BIT10|BIT11|BIT12|BIT13)               // (ROS)
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_PIC_MASK   (BIT14|BIT15)           // Port Indicator (RWS)
#define PORTSC_LWS        BIT16   // Link State Write Strobe       (RW)
#define PORTSC_CSC        BIT17   // Connect Status Change         (RW1CS)
#define PORTSC_PEC        BIT18   // Port Enable/Disable Change    (RW1CS)
#define PORTSC_WRC        BIT19   // Warm Port Reset Change        (RW1CS)
#define PORTSC_OCC        BIT20   // Over-Current Change           (RW1CS)
#define PORTSC_PRC        BIT21   // Port Reset Change             (RW1CS)
#define PORTSC_PLC        BIT22   // Port Link State Change        (RW1CS)
#define PORTSC_CEC        BIT23   // Config Error Change           (RW1CS)
#define PORTSC_CAS        BIT24   // Cold Attach Status            (RO)
#define PORTSC_WCE        BIT25   // Wake on Connect Enable        (RWS)
#define PORTSC_WDE        BIT26   // Wake on Disconnect Enable     (RWS)
#define PORTSC_WOE        BIT27   // Wake on Over-Current Enable   (RWS)
#define PORTSC_DR         BIT30   // Device Removable              (RO)
#define PORTSC_WPR        BIT31   // Warm Port Reset               (RW1S, USB3)

#define PORTSC_RW1CS_MASK  (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
                            PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

//
// ★★★ v2.0 重要修正 ★★★
//
// PORTSC 是個地雷暫存器：同一個 32-bit 裡混了 RO / RW / RW1C / RW1S 四種語意。
// 做讀-改-寫時，只能把「RO」和「RWS（一般讀寫）」這兩類位元寫回去；
// 其他一律要清成 0，否則寫回去就等於觸發它們。
//
// v1.1 的 bug：PORTSC_PRESERVE 只清掉 RW1CS_MASK / PR / WPR / LWS，
//              卻把 PED (bit1) 原封不動保留下來。但 PED 是 RW1CS —
//              「當 PED=1 時寫入 1 會使該 port 被 Disable」。
//              所以只要對一個已啟用的 port 做任何讀-改-寫，就會把它關掉，
//              PLS(bit5-8) 隨即掉到 Disabled(4)。這就是實測看到
//              「bit 5-8 修不好」的真正原因。
//
// v2.0 改用白名單（與 Linux drivers/usb/host/xhci-hub.c 的
// xhci_port_state_to_neutral() 完全一致）：只留 RO + RWS，其餘全部歸零。
// 寫入 RO 位元是無害的（硬體忽略），所以連同 RO 一起寫回最安全。
//
#define PORTSC_RO_MASK   (PORTSC_CCS | PORTSC_OCA | PORTSC_SPEED_MASK | PORTSC_DR)
#define PORTSC_RWS_MASK  (PORTSC_PLS_MASK | PORTSC_PP | PORTSC_PIC_MASK | \
                          PORTSC_WCE | PORTSC_WDE | PORTSC_WOE)

#define PORTSC_PRESERVE(portsc)  ((portsc) & (PORTSC_RO_MASK | PORTSC_RWS_MASK))

// PLS 狀態值
#define PLS_U0            0x0
#define PLS_U3            0x3
#define PLS_DISABLED      0x4
#define PLS_RXDETECT      0x5
#define PLS_INACTIVE      0x6
#define PLS_POLLING       0x7
#define PLS_COMPLIANCE    0xA

// ============================================================
//  程式參數
// ============================================================
#define MAX_XHCI_CONTROLLERS      8
#define MAX_PORTS_PER_CONTROLLER  32
#define MAX_TOTAL_PORTS           (MAX_XHCI_CONTROLLERS * MAX_PORTS_PER_CONTROLLER)
#define MAX_LOG_ENTRIES           1024
#define MAX_LOG_LINE_LEN          256

#define MAX_RECOVERY_RETRIES      3
#define RECOVERY_CLEAR_WAIT_MS    100
#define RECOVERY_PP_WAIT_MS       120   // PP=1 後等電源穩定
#define RECOVERY_RESET_WAIT_MS    200
#define RESET_POLL_TIMEOUT_MS     600   // 等 PR/WPR 自清
#define RESET_POLL_STEP_MS        10
#define PLS_SETTLE_WAIT_MS        150
#define HCRST_TIMEOUT_MS          1000

// ============================================================
//  資料結構
// ============================================================
typedef enum {
  LOG_INFO    = 0,
  LOG_WARNING = 1,
  LOG_ERROR   = 2,
  LOG_CRITICAL= 3,
} LOG_LEVEL;

//
// 每個 port 的協定資訊，以及它是從哪一筆 Supported Protocol Capability 推出來的。
// [9] 詳細診斷要把推導過程原封不動秀給使用者看。
//
typedef struct {
  UINT8   Major;        // 2=USB2, 3=USB3, 0=unknown
  UINT8   Minor;
  UINT32  CapOffset;    // 這筆 cap 在 BAR0 的哪個 offset 被找到
  UINT8   PortOffset;   // 該 cap 宣告的起始 port
  UINT8   PortCount;    // 該 cap 宣告的 port 數量
  CHAR8   Name[5];      // cap 的 4-byte Name String，通常是 "USB "
} PORT_PROTO_INFO;

typedef struct {
  UINTN                Segment;
  UINTN                Bus;
  UINTN                Device;
  UINTN                Function;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT32               Bar0Raw;
  UINT32               Bar1Raw;
  BOOLEAN              Bar64;
  UINT64               MmioBase;
  UINT8                CapLength;
  UINT16               HciVersion;
  UINT64               OperationalBase;
  UINT32               HcsParams1;
  UINT32               HccParams1;
  UINT8                MaxPorts;
  BOOLEAN              IsBootSource;
  PORT_PROTO_INFO      PortProto[MAX_PORTS_PER_CONTROLLER + 1];  // 1-based
  BOOLEAN              Valid;
} XHCI_CONTROLLER_INFO;

typedef struct {
  UINT8    ControllerIndex;
  UINT8    PortNumber;        // 1-based
  UINT8    ProtocolMajor;     // 2=USB2, 3=USB3, 0=unknown
  UINT32   LastPortsc;
  BOOLEAN  OcaSuspect;
  UINT8    RecoveryRetries;
  BOOLEAN  Valid;
} PORT_STATE;

typedef struct {
  UINT64    Timestamp;
  LOG_LEVEL Level;
  CHAR16    Message[MAX_LOG_LINE_LEN];
} LOG_ENTRY;

// ============================================================
//  全域狀態
// ============================================================
STATIC XHCI_CONTROLLER_INFO  gControllers[MAX_XHCI_CONTROLLERS];
STATIC UINTN                 gControllerCount = 0;

STATIC PORT_STATE            gPorts[MAX_TOTAL_PORTS];
STATIC UINTN                 gPortCount = 0;

STATIC LOG_ENTRY             gLogBuffer[MAX_LOG_ENTRIES];
STATIC UINTN                 gLogCount = 0;

STATIC BOOLEAN               gRunning = FALSE;

// 開機來源 controller（HCRST 保護用）
STATIC BOOLEAN               gBootCtrlValid = FALSE;
STATIC UINTN                 gBootSeg, gBootBus, gBootDev, gBootFunc;

// ============================================================
//  函式前置宣告
// ============================================================
EFI_STATUS   ScanXhciControllers    (VOID);
EFI_STATUS   ReadXhciCapability     (XHCI_CONTROLLER_INFO *Ctrl);
VOID         ParseSupportedProtocols(XHCI_CONTROLLER_INFO *Ctrl);
VOID         DetectBootController   (VOID);
EFI_STATUS   ReadMmio32             (XHCI_CONTROLLER_INFO *Ctrl, UINT64 BarOffset, UINT32 *Val);
EFI_STATUS   WriteMmio32            (XHCI_CONTROLLER_INFO *Ctrl, UINT64 BarOffset, UINT32 Val);
EFI_STATUS   ReadPortsc             (XHCI_CONTROLLER_INFO *Ctrl, UINT8 PortNum, UINT32 *Portsc);
EFI_STATUS   WritePortsc            (XHCI_CONTROLLER_INFO *Ctrl, UINT8 PortNum, UINT32 Value);
UINT64       PortscBarOffset        (XHCI_CONTROLLER_INFO *Ctrl, UINT8 PortNum);
CONST CHAR16 *PlsToString           (UINT8 Pls);
CONST CHAR16 *SpeedToString         (UINT8 Speed);
VOID         ParseAndPrintPortsc    (UINT32 Portsc);
VOID         AppLog                 (LOG_LEVEL Level, CONST CHAR16 *Format, ...);
VOID         PrintBanner            (VOID);
VOID         PrintMainMenu          (VOID);
EFI_STATUS   HandleMenuInput        (VOID);
UINTN        ScanForOc              (BOOLEAN PrintAll);
EFI_STATUS   DoPortRecovery         (UINTN PortIdx);
EFI_STATUS   DoControllerReset      (UINTN CtrlIdx);
VOID         RecoverAllOcPorts      (VOID);
EFI_STATUS   ExportLogToFile        (VOID);
VOID         PrintPortDetail        (UINTN PortIdx);
BOOLEAN      FindFsOnPort           (XHCI_CONTROLLER_INFO *Ctrl, UINT8 PortNum, UINTN *FsIndex);
VOID         PrintPortDiagnosis     (UINT32 Portsc, UINT8 ProtoMajor, BOOLEAN IsBootPort);
BOOLEAN      ConfirmAction          (CONST CHAR16 *Prompt);
BOOLEAN      DoubleConfirmAction    (CONST CHAR16 *Prompt);
EFI_STATUS   GetTimestampMs         (UINT64 *Ms);
EFI_STATUS   WaitMs                 (UINTN Ms);
CHAR16       ReadSingleKey          (VOID);
UINTN        ReadUintFromUser       (UINTN MaxDigits);
VOID         PauseForKey            (VOID);

// ============================================================
//  ENTRY POINT
// ============================================================
EFI_STATUS
EFIAPI
UsbOcRecoverMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  gST->ConOut->ClearScreen (gST->ConOut);
  PrintBanner ();

  DetectBootController ();
  if (gBootCtrlValid) {
    AppLog (LOG_INFO, L"[INIT] 開機來源 controller = Seg%d Bus%02X Dev%02X Func%X",
            gBootSeg, gBootBus, gBootDev, gBootFunc);
  }

  Status = ScanXhciControllers ();
  if (EFI_ERROR (Status)) {
    Print (L"\r\n[錯誤] 掃描 xHCI 控制器失敗: %r\r\n", Status);
    return Status;
  }

  if (gControllerCount == 0) {
    Print (L"\r\n[警告] 未找到任何 xHCI 控制器。\r\n");
    Print (L"  可能：未安裝 / 被 BIOS 停用 / PCI 枚舉未完成。\r\n");
    return EFI_NOT_FOUND;
  }

  Print (L"\r\n找到 %d 個 xHCI 控制器，共 %d 個 port。啟動掃描：\r\n",
         gControllerCount, gPortCount);
  ScanForOc (TRUE);

  gRunning = TRUE;
  while (gRunning) {
    PrintMainMenu ();
    Status = HandleMenuInput ();
    if (EFI_ERROR (Status) && Status != EFI_ABORTED) {
      AppLog (LOG_ERROR, L"[MENU] HandleMenuInput 回傳錯誤: %r", Status);
    }
  }

  Print (L"\r\n是否要匯出 log 到檔案？(Y/N): ");
  {
    CHAR16  ExitCh = ReadSingleKey ();
    Print (L"%c\r\n", ExitCh);
    if (ExitCh == L'Y' || ExitCh == L'y') {
      ExportLogToFile ();
    }
  }
  Print (L"\r\n[UsbOcRecover] 程式結束。\r\n");
  return EFI_SUCCESS;
}

// ============================================================
//  偵測開機來源 controller（HCRST 保護用）
// ============================================================
VOID
DetectBootController (
  VOID
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  EFI_DEVICE_PATH_PROTOCOL   *Dp;
  EFI_HANDLE                 PciHandle;
  EFI_PCI_IO_PROTOCOL        *BootPci;

  gBootCtrlValid = FALSE;

  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status) || Li->DeviceHandle == NULL) {
    return;
  }

  Dp = DevicePathFromHandle (Li->DeviceHandle);
  if (Dp == NULL) {
    return;
  }

  Status = gBS->LocateDevicePath (&gEfiPciIoProtocolGuid, &Dp, &PciHandle);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = gBS->HandleProtocol (PciHandle, &gEfiPciIoProtocolGuid, (VOID **)&BootPci);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = BootPci->GetLocation (BootPci, &gBootSeg, &gBootBus, &gBootDev, &gBootFunc);
  if (!EFI_ERROR (Status)) {
    gBootCtrlValid = TRUE;
  }
}

// ============================================================
//  xHCI 控制器掃描
// ============================================================
EFI_STATUS
ScanXhciControllers (
  VOID
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           *HandleBuffer;
  UINTN                HandleCount;
  UINTN                Idx;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT8                ClassCode[3];   // [0]=ProgIF, [1]=SubClass, [2]=Class
  UINT32               Bar0, Bar1;
  UINT64               MmioBase;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiPciIoProtocolGuid, NULL,
                  &HandleCount, &HandleBuffer);
  if (EFI_ERROR (Status)) {
    AppLog (LOG_ERROR, L"LocateHandleBuffer(PciIo) 失敗: %r", Status);
    return Status;
  }

  AppLog (LOG_INFO, L"[SCAN] 找到 %d 個 PCI 裝置，開始篩選 xHCI...", HandleCount);

  for (Idx = 0; Idx < HandleCount; Idx++) {
    XHCI_CONTROLLER_INFO *Ctrl;
    UINT8                PortNum;

    Status = gBS->HandleProtocol (HandleBuffer[Idx], &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, 0x09, 3, ClassCode);
    if (EFI_ERROR (Status)) {
      continue;
    }
    if (ClassCode[2] != PCI_CLASS_SERIAL ||
        ClassCode[1] != PCI_SUBCLASS_USB ||
        ClassCode[0] != PCI_IF_XHCI) {
      continue;
    }

    if (gControllerCount >= MAX_XHCI_CONTROLLERS) {
      AppLog (LOG_WARNING, L"[SCAN] 超過最大控制器數量，忽略後續");
      break;
    }

    Ctrl = &gControllers[gControllerCount];
    ZeroMem (Ctrl, sizeof (XHCI_CONTROLLER_INFO));
    Ctrl->PciIo = PciIo;
    Ctrl->Valid = TRUE;

    Status = PciIo->GetLocation (PciIo, &Ctrl->Segment, &Ctrl->Bus, &Ctrl->Device, &Ctrl->Function);
    if (EFI_ERROR (Status)) {
      AppLog (LOG_WARNING, L"[SCAN] GetLocation 失敗: %r", Status);
    }

    Ctrl->IsBootSource = (gBootCtrlValid &&
                          Ctrl->Segment  == gBootSeg &&
                          Ctrl->Bus      == gBootBus &&
                          Ctrl->Device   == gBootDev &&
                          Ctrl->Function == gBootFunc);

    // BAR0（保留原始值，[9] 詳細診斷要用來說明位址怎麼算的）
    Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BAR0_OFFSET, 1, &Bar0);
    if (EFI_ERROR (Status)) {
      AppLog (LOG_ERROR, L"[SCAN] 讀取 BAR0 失敗: %r", Status);
      Ctrl->Valid = FALSE;
      continue;
    }
    Ctrl->Bar0Raw = Bar0;
    MmioBase      = Bar0 & 0xFFFFFFF0U;
    Ctrl->Bar64   = ((Bar0 & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_TYPE_64);
    if (Ctrl->Bar64) {
      Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BAR1_OFFSET, 1, &Bar1);
      if (!EFI_ERROR (Status)) {
        Ctrl->Bar1Raw = Bar1;
        MmioBase     |= ((UINT64)Bar1 << 32);
      }
    }
    Ctrl->MmioBase = MmioBase;

    Status = ReadXhciCapability (Ctrl);
    if (EFI_ERROR (Status)) {
      AppLog (LOG_ERROR, L"[SCAN] 讀取 Capability 失敗: %r，跳過", Status);
      Ctrl->Valid = FALSE;
      continue;
    }

    ParseSupportedProtocols (Ctrl);

    Print (L"  [xHCI #%d] Bus%02X Dev%02X Func%X  MMIO=0x%LX  MaxPorts=%d%s\r\n",
           gControllerCount, Ctrl->Bus, Ctrl->Device, Ctrl->Function,
           Ctrl->MmioBase, Ctrl->MaxPorts,
           Ctrl->IsBootSource ? L"  <= 開機來源" : L"");

    for (PortNum = 1; PortNum <= Ctrl->MaxPorts; PortNum++) {
      PORT_STATE *Port;
      if (gPortCount >= MAX_TOTAL_PORTS) {
        break;
      }
      Port = &gPorts[gPortCount];
      ZeroMem (Port, sizeof (PORT_STATE));
      Port->ControllerIndex = (UINT8)gControllerCount;
      Port->PortNumber      = PortNum;
      Port->ProtocolMajor   = Ctrl->PortProto[PortNum].Major;
      Port->Valid           = TRUE;
      gPortCount++;
    }

    gControllerCount++;
  }

  FreePool (HandleBuffer);
  return EFI_SUCCESS;
}

EFI_STATUS
ReadXhciCapability (
  IN OUT XHCI_CONTROLLER_INFO  *Ctrl
  )
{
  EFI_STATUS  Status;
  UINT8       CapLength8;
  UINT16      HciVer;
  UINT32      HcsParams1;
  UINT32      HccParams1;
  UINT8       MaxPorts;

  Status = Ctrl->PciIo->Mem.Read (Ctrl->PciIo, EfiPciIoWidthUint8, 0,
                                  XHCI_CAPLENGTH_OFFSET, 1, &CapLength8);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  if (CapLength8 < 0x10 || (CapLength8 & 0x03) != 0) {
    AppLog (LOG_WARNING, L"[CAP] CAPLENGTH=0x%02X 不符規範，仍嘗試繼續", CapLength8);
  }
  Ctrl->CapLength       = CapLength8;
  Ctrl->OperationalBase = Ctrl->MmioBase + CapLength8;

  if (!EFI_ERROR (Ctrl->PciIo->Mem.Read (Ctrl->PciIo, EfiPciIoWidthUint16, 0,
                                         XHCI_HCIVERSION_OFFSET, 1, &HciVer))) {
    Ctrl->HciVersion = HciVer;
  }

  Status = Ctrl->PciIo->Mem.Read (Ctrl->PciIo, EfiPciIoWidthUint32, 0,
                                  XHCI_HCSPARAMS1_OFFSET, 1, &HcsParams1);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  Ctrl->HcsParams1 = HcsParams1;

  Status = Ctrl->PciIo->Mem.Read (Ctrl->PciIo, EfiPciIoWidthUint32, 0,
                                  XHCI_HCCPARAMS1_OFFSET, 1, &HccParams1);
  if (EFI_ERROR (Status)) {
    HccParams1 = 0;
  }
  Ctrl->HccParams1 = HccParams1;

  MaxPorts = (UINT8)((HcsParams1 & HCSPARAMS1_MAXPORTS_MASK) >> HCSPARAMS1_MAXPORTS_SHIFT);
  if (MaxPorts == 0) {
    AppLog (LOG_WARNING, L"[CAP] MaxPorts=0，設為 1");
    MaxPorts = 1;
  }
  if (MaxPorts > MAX_PORTS_PER_CONTROLLER) {
    MaxPorts = MAX_PORTS_PER_CONTROLLER;
  }
  Ctrl->MaxPorts = MaxPorts;
  return EFI_SUCCESS;
}

//
// 走訪 Extended Capability 串鏈，從 Supported Protocol Capability (ID=0x02)
// 標記每個 port 是 USB2 還是 USB3，並記下推導來源供 [9] 顯示。
//
VOID
ParseSupportedProtocols (
  IN OUT XHCI_CONTROLLER_INFO  *Ctrl
  )
{
  UINT32  XecpOff;
  UINT32  Dword0, Dword1, Dword2;
  UINT8   CapId, NextPtr, MajorRev, MinorRev, PortOff, PortCnt, i;

  ZeroMem (Ctrl->PortProto, sizeof (Ctrl->PortProto));

  XecpOff = ((Ctrl->HccParams1 >> HCCPARAMS1_XECP_SHIFT) & 0xFFFF) << 2;
  if (XecpOff == 0) {
    return;
  }

  while (XecpOff != 0) {
    if (EFI_ERROR (ReadMmio32 (Ctrl, XecpOff, &Dword0))) {
      break;
    }
    CapId   = (UINT8)(Dword0 & 0xFF);
    NextPtr = (UINT8)((Dword0 >> 8) & 0xFF);

    if (CapId == XHCI_ECAP_ID_SUPPORTED_PROTOCOL) {
      MinorRev = (UINT8)((Dword0 >> 16) & 0xFF);
      MajorRev = (UINT8)((Dword0 >> 24) & 0xFF);

      if (EFI_ERROR (ReadMmio32 (Ctrl, XecpOff + 4, &Dword1))) {
        Dword1 = 0;
      }
      if (!EFI_ERROR (ReadMmio32 (Ctrl, XecpOff + 8, &Dword2))) {
        PortOff = (UINT8)(Dword2 & 0xFF);
        PortCnt = (UINT8)((Dword2 >> 8) & 0xFF);
        for (i = 0; i < PortCnt; i++) {
          UINT8 pn = (UINT8)(PortOff + i);
          if (pn >= 1 && pn <= MAX_PORTS_PER_CONTROLLER) {
            PORT_PROTO_INFO *Pi = &Ctrl->PortProto[pn];
            Pi->Major      = MajorRev;
            Pi->Minor      = MinorRev;
            Pi->CapOffset  = XecpOff;
            Pi->PortOffset = PortOff;
            Pi->PortCount  = PortCnt;
            Pi->Name[0]    = (CHAR8)(Dword1 & 0xFF);
            Pi->Name[1]    = (CHAR8)((Dword1 >> 8) & 0xFF);
            Pi->Name[2]    = (CHAR8)((Dword1 >> 16) & 0xFF);
            Pi->Name[3]    = (CHAR8)((Dword1 >> 24) & 0xFF);
            Pi->Name[4]    = '\0';
          }
        }
      }
    }

    if (NextPtr == 0) {
      break;
    }
    XecpOff += ((UINT32)NextPtr) << 2;
  }
}

// ============================================================
//  MMIO 讀寫（offset 相對 BAR0）
// ============================================================
EFI_STATUS
ReadMmio32 (
  IN  XHCI_CONTROLLER_INFO  *Ctrl,
  IN  UINT64                BarOffset,
  OUT UINT32                *Val
  )
{
  return Ctrl->PciIo->Mem.Read (Ctrl->PciIo, EfiPciIoWidthUint32, 0, BarOffset, 1, Val);
}

EFI_STATUS
WriteMmio32 (
  IN XHCI_CONTROLLER_INFO  *Ctrl,
  IN UINT64                BarOffset,
  IN UINT32                Val
  )
{
  return Ctrl->PciIo->Mem.Write (Ctrl->PciIo, EfiPciIoWidthUint32, 0, BarOffset, 1, &Val);
}

UINT64
PortscBarOffset (
  IN XHCI_CONTROLLER_INFO  *Ctrl,
  IN UINT8                 PortNum
  )
{
  return (UINT64)Ctrl->CapLength + XHCI_PORTSC_BASE_OFFSET
         + XHCI_PORTSC_PORT_STRIDE * (PortNum - 1);
}

EFI_STATUS
ReadPortsc (
  IN  XHCI_CONTROLLER_INFO  *Ctrl,
  IN  UINT8                 PortNum,
  OUT UINT32                *Portsc
  )
{
  if (PortNum < 1 || PortNum > Ctrl->MaxPorts) {
    return EFI_INVALID_PARAMETER;
  }
  return ReadMmio32 (Ctrl, PortscBarOffset (Ctrl, PortNum), Portsc);
}

EFI_STATUS
WritePortsc (
  IN XHCI_CONTROLLER_INFO  *Ctrl,
  IN UINT8                 PortNum,
  IN UINT32                Value
  )
{
  AppLog (LOG_INFO, L"[WRITE] Ctrl#%d Port%d PORTSC <- 0x%08X",
          (UINTN)(Ctrl - gControllers), PortNum, Value);
  return WriteMmio32 (Ctrl, PortscBarOffset (Ctrl, PortNum), Value);
}

// ============================================================
//  PORTSC 解析與顯示
// ============================================================
CONST CHAR16 *
PlsToString (
  IN UINT8  Pls
  )
{
  switch (Pls) {
    case 0x0: return L"U0";
    case 0x1: return L"U1";
    case 0x2: return L"U2";
    case 0x3: return L"U3";
    case 0x4: return L"Disabled";
    case 0x5: return L"RxDetect";
    case 0x6: return L"Inactive";
    case 0x7: return L"Polling";
    case 0x8: return L"Recovery";
    case 0x9: return L"HotReset";
    case 0xA: return L"Compliance";
    case 0xB: return L"TestMode";
    case 0xF: return L"Resume";
    default:  return L"保留值";
  }
}

CONST CHAR16 *
SpeedToString (
  IN UINT8  Speed
  )
{
  switch (Speed) {
    case 0:  return L"未連線";
    case 1:  return L"Full-Speed 12Mb/s";
    case 2:  return L"Low-Speed 1.5Mb/s";
    case 3:  return L"High-Speed 480Mb/s";
    case 4:  return L"SuperSpeed 5Gb/s";
    case 5:  return L"SuperSpeedPlus 10Gb/s";
    default: return L"廠商自訂";
  }
}

VOID
ParseAndPrintPortsc (
  IN UINT32  Portsc
  )
{
  UINT8 Pls   = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);
  UINT8 Speed = (UINT8)((Portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);

  if (Portsc & PORTSC_OCA) {
    Print (L"    !! OCA=1 [過電流中!]\r\n");
  }
  Print (L"    PORTSC=0x%08X  CCS=%d PED=%d OCA=%d PR=%d PP=%d OCC=%d PLS=%d(%s) Spd=%d\r\n",
         Portsc,
         (Portsc & PORTSC_CCS) ? 1 : 0,
         (Portsc & PORTSC_PED) ? 1 : 0,
         (Portsc & PORTSC_OCA) ? 1 : 0,
         (Portsc & PORTSC_PR)  ? 1 : 0,
         (Portsc & PORTSC_PP)  ? 1 : 0,
         (Portsc & PORTSC_OCC) ? 1 : 0,
         Pls, PlsToString (Pls), Speed);
}

// ============================================================
//  [1] 快速 OC 掃描
// ============================================================
UINTN
ScanForOc (
  IN BOOLEAN  PrintAll
  )
{
  EFI_STATUS  Status;
  UINTN       Pi;
  UINT32      Portsc;
  UINTN       OcCount = 0;

  Print (L"\r\n  Idx Ctrl Port Proto  PORTSC      OCA OCC PP  PLS       狀態\r\n");
  Print (L"  --------------------------------------------------------------------\r\n");

  for (Pi = 0; Pi < gPortCount; Pi++) {
    PORT_STATE           *Port = &gPorts[Pi];
    XHCI_CONTROLLER_INFO *Ctrl = &gControllers[Port->ControllerIndex];
    UINT8   Pls;
    BOOLEAN Oca, Occ, Pp;
    CONST CHAR16 *St;

    if (!Port->Valid || !Ctrl->Valid) {
      continue;
    }
    Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Port->LastPortsc = Portsc;

    Oca = (Portsc & PORTSC_OCA) != 0;
    Occ = (Portsc & PORTSC_OCC) != 0;
    Pp  = (Portsc & PORTSC_PP)  != 0;
    Pls = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);

    if (Oca) {
      St = L"[!! 過電流進行中]"; OcCount++;
    } else if (Occ) {
      St = L"[! 曾過電流(可復原)]"; OcCount++;
    } else if (!Pp && Pls == PLS_DISABLED) {
      St = L"[電源關閉/Disabled]"; OcCount++;
    } else {
      St = L"[正常]";
    }

    if (PrintAll || Oca || Occ || (!Pp && Pls == PLS_DISABLED)) {
      Print (L"  [%2d]  #%d  P%-2d  USB%d  0x%08X  %d   %d   %d  %-8s %s\r\n",
             Pi, Port->ControllerIndex, Port->PortNumber,
             Port->ProtocolMajor ? Port->ProtocolMajor : 0,
             Portsc, Oca, Occ, Pp, PlsToString (Pls), St);
    }
  }

  if (OcCount == 0) {
    Print (L"\r\n  ==> 未偵測到 Over-Current，所有 port 正常。\r\n");
  } else {
    Print (L"\r\n  ==> 偵測到 %d 個 port 有 OC / 電源未回復。按 [5] 可一鍵復原。\r\n", OcCount);
    AppLog (LOG_WARNING, L"[SCAN-OC] 偵測到 %d 個 OC/未回復 port", OcCount);
  }
  return OcCount;
}

// ============================================================
//  Port 級 Recovery
//  清 OCC -> PP=1 -> WPR/PR -> 清 change bits -> 必要時導 PLS -> 驗證
// ============================================================
EFI_STATUS
DoPortRecovery (
  IN UINTN  PortIdx
  )
{
  EFI_STATUS            Status;
  PORT_STATE            *Port;
  XHCI_CONTROLLER_INFO  *Ctrl;
  UINT32                Portsc, Base, WriteVal;
  UINTN                 Waited;
  UINT8                 Pls;
  BOOLEAN               UseWarmReset;

  if (PortIdx >= gPortCount) {
    return EFI_INVALID_PARAMETER;
  }
  Port = &gPorts[PortIdx];
  Ctrl = &gControllers[Port->ControllerIndex];
  if (!Port->Valid || !Ctrl->Valid) {
    return EFI_ABORTED;
  }

  if (Port->RecoveryRetries >= MAX_RECOVERY_RETRIES) {
    Print (L"\r\n  [Recovery] Ctrl#%d Port%d 已達最大重試次數 %d。\r\n",
           Port->ControllerIndex, Port->PortNumber, MAX_RECOVERY_RETRIES);
    return EFI_ABORTED;
  }

  Print (L"\r\n---- Recovery: Ctrl#%d Port %d (USB%d) ----\r\n",
         Port->ControllerIndex, Port->PortNumber,
         Port->ProtocolMajor ? Port->ProtocolMajor : 3);

  // Step 1: 安全確認 OCA==0
  Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  ParseAndPrintPortsc (Portsc);

  if (Portsc & PORTSC_OCA) {
    Print (L"\r\n  *** 安全中止：OCA==1，過電流仍在！請先拔除裝置。***\r\n");
    AppLog (LOG_CRITICAL, L"[RECOVERY] OCA==1 安全中止 Ctrl#%d Port%d",
            Port->ControllerIndex, Port->PortNumber);
    return EFI_ABORTED;
  }

  // Step 2: 雙重確認
  if (!DoubleConfirmAction (L"  即將寫入 PORTSC（清 OCC + 上電 + Reset），確認執行")) {
    Print (L"  使用者取消。\r\n");
    return EFI_ABORTED;
  }
  Port->RecoveryRetries++;

  // Step 3: 清 OCC
  Print (L"  [1/5] 清除 OCC...\r\n");
  Base     = PORTSC_PRESERVE (Portsc);
  WriteVal = Base | PORTSC_OCC;
  Status   = WritePortsc (Ctrl, Port->PortNumber, WriteVal);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  WaitMs (RECOVERY_CLEAR_WAIT_MS);

  // Step 4: PP=1 重新上電
  Print (L"  [2/5] 設 PP=1（重新上電）...\r\n");
  Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  if (Portsc & PORTSC_OCA) {
    Print (L"  *** 上電前 OCA 再現，中止。***\r\n");
    Port->OcaSuspect = TRUE;
    return EFI_ABORTED;
  }
  Base     = PORTSC_PRESERVE (Portsc);
  WriteVal = Base | PORTSC_PP;
  Status   = WritePortsc (Ctrl, Port->PortNumber, WriteVal);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  WaitMs (RECOVERY_PP_WAIT_MS);

  // Step 5: Warm Port Reset (USB3) 或 Port Reset (USB2)
  UseWarmReset = (Port->ProtocolMajor != 2);   // 未知或 USB3 -> Warm Reset
  Print (L"  [3/5] 下 %s ...\r\n", UseWarmReset ? L"Warm Port Reset(bit31)" : L"Port Reset(bit4)");

  Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  if (Portsc & PORTSC_OCA) {
    Print (L"  *** reset 前 OCA 再現，中止。***\r\n");
    Port->OcaSuspect = TRUE;
    return EFI_ABORTED;
  }
  Base     = PORTSC_PRESERVE (Portsc) | PORTSC_PP;
  WriteVal = Base | (UseWarmReset ? PORTSC_WPR : PORTSC_PR);
  Status   = WritePortsc (Ctrl, Port->PortNumber, WriteVal);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Waited = 0;
  while (Waited < RESET_POLL_TIMEOUT_MS) {
    WaitMs (RESET_POLL_STEP_MS);
    Waited += RESET_POLL_STEP_MS;
    Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
    if (EFI_ERROR (Status)) {
      break;
    }
    if ((Portsc & (PORTSC_PR | PORTSC_WPR)) == 0) {
      break;   // reset 完成
    }
  }

  // Step 6: 清 change bits
  Print (L"  [4/5] 清除 change bits（reset 用了 %d ms）...\r\n", Waited);
  Base     = PORTSC_PRESERVE (Portsc) | PORTSC_PP;
  WriteVal = Base | (Portsc & PORTSC_RW1CS_MASK);
  WritePortsc (Ctrl, Port->PortNumber, WriteVal);
  WaitMs (RECOVERY_RESET_WAIT_MS);

  //
  // Step 7: PLS 導向（v2.0 新增）
  //
  // reset 之後 link 有時仍卡在 Disabled(4)。xHCI Spec Table 5-27 允許用
  // LWS(bit16)=1 搭配 PLS 欄位做 directed link state change，把 Disabled
  // 導回 RxDetect(5) 重新偵測。注意 PORTSC_PRESERVE 會保留舊 PLS 值，
  // 所以要先把 PLS 欄位清掉再填新值，否則等於寫回原本的 Disabled。
  //
  Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  Pls = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);

  if ((Portsc & PORTSC_OCA) == 0 && (Pls == PLS_DISABLED || Pls == PLS_INACTIVE)) {
    Print (L"  [5/5] PLS 仍為 %s，導向 RxDetect...\r\n", PlsToString (Pls));
    Base     = (PORTSC_PRESERVE (Portsc) & ~(UINT32)PORTSC_PLS_MASK) | PORTSC_PP;
    WriteVal = Base | PORTSC_LWS | ((UINT32)PLS_RXDETECT << PORTSC_PLS_SHIFT);
    WritePortsc (Ctrl, Port->PortNumber, WriteVal);
    WaitMs (PLS_SETTLE_WAIT_MS);

    // 導向會產生 PLC，順手清掉
    if (!EFI_ERROR (ReadPortsc (Ctrl, Port->PortNumber, &Portsc))) {
      if (Portsc & PORTSC_PLC) {
        Base = PORTSC_PRESERVE (Portsc) | PORTSC_PP;
        WritePortsc (Ctrl, Port->PortNumber, Base | PORTSC_PLC);
        WaitMs (RECOVERY_CLEAR_WAIT_MS);
      }
    }
  } else {
    Print (L"  [5/5] PLS=%s，不需導向。\r\n", PlsToString (Pls));
  }

  // Step 8: 驗證
  Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  Print (L"  驗證：");
  ParseAndPrintPortsc (Portsc);
  Pls = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);

  if (Portsc & PORTSC_OCA) {
    Print (L"  ==> FAIL：OCA 再現，硬體可能仍異常。\r\n");
    Port->OcaSuspect = TRUE;
    return EFI_DEVICE_ERROR;
  }

  if ((Portsc & PORTSC_PP) && Pls != PLS_DISABLED) {
    Print (L"  ==> RECOVERED（PP=1、PLS=%s，未 AC off 已復原）\r\n", PlsToString (Pls));
    AppLog (LOG_INFO, L"[RECOVERY] RECOVERED Ctrl#%d Port%d PORTSC=0x%08X",
            Port->ControllerIndex, Port->PortNumber, Portsc);
    Port->OcaSuspect      = FALSE;
    Port->RecoveryRetries = 0;
    return EFI_SUCCESS;
  }

  Print (L"  ==> 未完全復原（PP=%d PLS=%s）。\r\n",
         (Portsc & PORTSC_PP) ? 1 : 0, PlsToString (Pls));
  AppLog (LOG_WARNING, L"[RECOVERY] 未完全復原 Ctrl#%d Port%d PORTSC=0x%08X",
          Port->ControllerIndex, Port->PortNumber, Portsc);
  return EFI_DEVICE_ERROR;
}

// ============================================================
//  Controller 級 HCRST
//  不放在主選單；只在 port 級復原失敗時，當作最後手段就地詢問。
// ============================================================
EFI_STATUS
DoControllerReset (
  IN UINTN  CtrlIdx
  )
{
  EFI_STATUS            Status;
  XHCI_CONTROLLER_INFO  *Ctrl;
  UINT32                Cmd, Sts;
  UINTN                 Waited;
  UINT8                 PortNum;

  if (CtrlIdx >= gControllerCount) {
    return EFI_INVALID_PARAMETER;
  }
  Ctrl = &gControllers[CtrlIdx];
  if (!Ctrl->Valid) {
    return EFI_ABORTED;
  }

  Print (L"\r\n---- Controller HCRST: Ctrl#%d (Bus%02X Dev%02X Func%X) ----\r\n",
         CtrlIdx, Ctrl->Bus, Ctrl->Device, Ctrl->Function);
  Print (L"  警告：HCRST 會重置整個 controller 底下所有 port。\r\n");

  if (Ctrl->IsBootSource) {
    Print (L"  *** 此 controller 是開機/工具來源，HCRST 會中斷本工具。已阻止。***\r\n");
    AppLog (LOG_CRITICAL, L"[HCRST] 阻止：Ctrl#%d 為開機來源", CtrlIdx);
    return EFI_ACCESS_DENIED;
  }

  if (!DoubleConfirmAction (L"  確認對此 controller 執行 HCRST")) {
    Print (L"  使用者取消。\r\n");
    return EFI_ABORTED;
  }

  // 1) Halt：清 R/S，等 HCH=1
  Status = ReadMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBCMD_OFFSET, &Cmd);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  WriteMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBCMD_OFFSET, Cmd & ~(UINT32)USBCMD_RS);

  Waited = 0;
  while (Waited < HCRST_TIMEOUT_MS) {
    WaitMs (RESET_POLL_STEP_MS);
    Waited += RESET_POLL_STEP_MS;
    if (EFI_ERROR (ReadMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBSTS_OFFSET, &Sts))) {
      break;
    }
    if (Sts & USBSTS_HCH) {
      break;
    }
  }
  Print (L"  [1] Halt 完成（%d ms）。\r\n", Waited);

  // 2) HCRST=1，等自清 + CNR=0
  Status = ReadMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBCMD_OFFSET, &Cmd);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }
  WriteMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBCMD_OFFSET, Cmd | USBCMD_HCRST);
  AppLog (LOG_WARNING, L"[HCRST] Ctrl#%d 下 HCRST", CtrlIdx);

  Waited = 0;
  while (Waited < HCRST_TIMEOUT_MS) {
    WaitMs (RESET_POLL_STEP_MS);
    Waited += RESET_POLL_STEP_MS;
    if (EFI_ERROR (ReadMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBCMD_OFFSET, &Cmd))) {
      break;
    }
    if (EFI_ERROR (ReadMmio32 (Ctrl, (UINT64)Ctrl->CapLength + XHCI_USBSTS_OFFSET, &Sts))) {
      break;
    }
    if ((Cmd & USBCMD_HCRST) == 0 && (Sts & USBSTS_CNR) == 0) {
      break;
    }
  }
  Print (L"  [2] HCRST 完成（%d ms）。\r\n", Waited);

  // 3) 所有 port 重新上電
  Print (L"  [3] 對所有 port 設 PP=1...\r\n");
  for (PortNum = 1; PortNum <= Ctrl->MaxPorts; PortNum++) {
    UINT32 P, Base;
    if (EFI_ERROR (ReadPortsc (Ctrl, PortNum, &P))) {
      continue;
    }
    Base = PORTSC_PRESERVE (P);
    WritePortsc (Ctrl, PortNum, Base | PORTSC_PP);
  }
  WaitMs (RECOVERY_PP_WAIT_MS);

  Print (L"  ==> HCRST 完成，完整枚舉由 OS/下次開機的 xHCI 驅動接手。\r\n");
  AppLog (LOG_INFO, L"[HCRST] Ctrl#%d 完成", CtrlIdx);
  return EFI_SUCCESS;
}

// ============================================================
//  [5] 一鍵復原所有 OC ports
// ============================================================
VOID
RecoverAllOcPorts (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Pi;
  UINT32      Portsc;
  UINTN       Handled = 0;
  UINTN       Failed  = 0;
  UINTN       FailedCtrl = 0;
  BOOLEAN     HaveFailedCtrl = FALSE;

  Print (L"\r\n  [一鍵復原] 掃描並復原所有可復原的 OC port...\r\n");

  for (Pi = 0; Pi < gPortCount; Pi++) {
    PORT_STATE           *Port = &gPorts[Pi];
    XHCI_CONTROLLER_INFO *Ctrl = &gControllers[Port->ControllerIndex];
    UINT8   Pls;

    if (!Port->Valid || !Ctrl->Valid) {
      continue;
    }
    if (EFI_ERROR (ReadPortsc (Ctrl, Port->PortNumber, &Portsc))) {
      continue;
    }
    Pls = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);

    if (Portsc & PORTSC_OCA) {
      Print (L"  Ctrl#%d Port%d: OCA=1，跳過（請先拔除裝置）。\r\n",
             Port->ControllerIndex, Port->PortNumber);
      continue;
    }
    if ((Portsc & PORTSC_OCC) || (!(Portsc & PORTSC_PP) && Pls == PLS_DISABLED)) {
      Status = DoPortRecovery (Pi);
      Handled++;
      if (EFI_ERROR (Status) && Status != EFI_ABORTED) {
        Failed++;
        if (!Ctrl->IsBootSource) {
          FailedCtrl     = Port->ControllerIndex;
          HaveFailedCtrl = TRUE;
        }
      }
    }
  }

  if (Handled == 0) {
    Print (L"  沒有需要復原的 port。\r\n");
    return;
  }

  Print (L"\r\n  [一鍵復原] 處理 %d 個 port，其中 %d 個未成功。\r\n", Handled, Failed);

  //
  // Port 級救不回來時的最後手段。不放進主選單，避免選單變長；
  // 只在真的需要時才問，否則使用者的下一步只剩拔 AC。
  //
  if (Failed > 0 && HaveFailedCtrl) {
    Print (L"\r\n  Port 級復原無效。最後手段是對 Ctrl#%d 下 HCRST（重置整顆控制器）。\r\n",
           FailedCtrl);
    if (ConfirmAction (L"  要試 HCRST 嗎")) {
      DoControllerReset (FailedCtrl);
    } else {
      Print (L"  略過。若仍要復原，只能關機拔 AC 電源。\r\n");
    }
  } else if (Failed > 0) {
    Print (L"  這些 port 在開機來源 controller 上，無法用 HCRST（會中斷本工具）。\r\n");
    Print (L"  若仍要復原，請改從其他 controller 的 USB 孔開機，或關機拔 AC 電源。\r\n");
  }
}

// ============================================================
//  [9] Port 詳細診斷
// ============================================================

//
// 找出掛在指定 (Ctrl, PortNum) 上、且帶有 SimpleFileSystem 的裝置，
// 回傳它在本工具枚舉順序中的索引（通常對應 UEFI Shell 的 fsN:）。
//
BOOLEAN
FindFsOnPort (
  IN  XHCI_CONTROLLER_INFO  *Ctrl,
  IN  UINT8                 PortNum,
  OUT UINTN                 *FsIndex
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *FsHandles;
  UINTN                     FsCount, i;
  BOOLEAN                   Found = FALSE;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &FsCount, &FsHandles);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  for (i = 0; i < FsCount; i++) {
    EFI_DEVICE_PATH_PROTOCOL  *Dp, *Walk;
    EFI_HANDLE                PciHandle;
    EFI_PCI_IO_PROTOCOL       *Pci;
    UINTN                     Seg, Bus, Dev, Func;

    Dp = DevicePathFromHandle (FsHandles[i]);
    if (Dp == NULL) {
      continue;
    }

    //
    // LocateDevicePath 會把 Walk 推進到「已匹配部分的後面」，
    // 也就是剛好停在 PCI 節點之後 —— 接下來的節點就是 USB 拓撲。
    //
    Walk   = Dp;
    Status = gBS->LocateDevicePath (&gEfiPciIoProtocolGuid, &Walk, &PciHandle);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = gBS->HandleProtocol (PciHandle, &gEfiPciIoProtocolGuid, (VOID **)&Pci);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = Pci->GetLocation (Pci, &Seg, &Bus, &Dev, &Func);
    if (EFI_ERROR (Status)) {
      continue;
    }
    if (Seg != Ctrl->Segment || Bus != Ctrl->Bus ||
        Dev != Ctrl->Device  || Func != Ctrl->Function) {
      continue;
    }

    //
    // 第一個 USB 節點就是接在 root hub 上的那一層。
    // ParentPortNumber 是 0-based，PORTSC 的 port 編號是 1-based。
    //
    while (!IsDevicePathEnd (Walk)) {
      if (DevicePathType (Walk) == MESSAGING_DEVICE_PATH &&
          DevicePathSubType (Walk) == MSG_USB_DP) {
        USB_DEVICE_PATH *Usb = (USB_DEVICE_PATH *)Walk;
        if ((UINT8)(Usb->ParentPortNumber + 1) == PortNum) {
          *FsIndex = i;
          Found    = TRUE;
        }
        break;
      }
      Walk = NextDevicePathNode (Walk);
    }
    if (Found) {
      break;
    }
  }

  FreePool (FsHandles);
  return Found;
}

//
// 把 PORTSC 翻譯成「這個 port 到底發生什麼事」的白話說明。
//
VOID
PrintPortDiagnosis (
  IN UINT32   Portsc,
  IN UINT8    ProtoMajor,
  IN BOOLEAN  IsBootPort
  )
{
  BOOLEAN Ccs = (Portsc & PORTSC_CCS) != 0;
  BOOLEAN Ped = (Portsc & PORTSC_PED) != 0;
  BOOLEAN Oca = (Portsc & PORTSC_OCA) != 0;
  BOOLEAN Pp  = (Portsc & PORTSC_PP)  != 0;
  BOOLEAN Occ = (Portsc & PORTSC_OCC) != 0;
  UINT8   Pls = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);

  if (Oca) {
    Print (L"  【現況】!! 過電流進行中（OCA=1）\r\n");
    Print (L"  這個 port 的電流超過保護閾值，xHCI 硬體已自動切斷 VBUS 供電。\r\n");
    Print (L"  OCA 是唯讀的即時訊號：它還是 1，就代表過流來源「現在還在」——\r\n");
    Print (L"  短路的線材、故障裝置，或 port 內有異物把 VBUS 和 GND 接在一起。\r\n");
    Print (L"  【該做什麼】立刻拔掉這個 port 上的所有東西。本工具在 OCA=1 時\r\n");
    Print (L"  拒絕上電與 reset —— 硬上電只會再觸發一次保護。\r\n");
    return;
  }

  if (Occ) {
    Print (L"  【現況】曾過電流，來源已排除（OCA=0, OCC=1）  <= 這個可以救\r\n");
    Print (L"  事情的經過是：這個 port 某個時間點觸發了過流保護，硬體做了兩件事——\r\n");
    Print (L"    (1) 把 PP（Port Power, bit9）清成 0，切斷這個 port 的 VBUS\r\n");
    Print (L"    (2) 把 OCC（Over-Current Change, bit20）設成 1，記錄「發生過」\r\n");
    Print (L"  現在 OCA 已經是 0，代表過流來源不在了。但 OCC 是 RW1C 旗標，\r\n");
    Print (L"  沒有人寫 1 去清它、也沒有人把 PP 設回 1，所以這個 port 就一直是死的。\r\n");
    Print (L"  韌體/OS 通常只在冷開機重跑初始化時才會處理，這正是為什麼一般\r\n");
    Print (L"  只能關機拔 AC 電源才會恢復。本工具做的就是手動補上這兩步。\r\n");
    Print (L"  【該做什麼】回主選單按 [5] 一鍵復原。\r\n");
    return;
  }

  if (!Pp) {
    Print (L"  【現況】電源關閉（PP=0），但沒有 OC 記錄\r\n");
    Print (L"  這個 port 沒電。OCC=0 表示不是過流打掉的，比較可能是：\r\n");
    Print (L"    - BIOS 設定停用了這個 port\r\n");
    Print (L"    - 這個 port 根本沒接出來（主機板保留孔位）\r\n");
    Print (L"    - 之前的 OC 事件已被清掉，只剩 PP 沒補回來\r\n");
    Print (L"  【該做什麼】[5] 一鍵復原會嘗試把 PP 設回 1。若設不回去，\r\n");
    Print (L"  多半是 BIOS 層級停用，工具無能為力。\r\n");
    return;
  }

  if (Pls == PLS_INACTIVE) {
    Print (L"  【現況】連結異常（PLS=Inactive）\r\n");
    Print (L"  有電（PP=1），但 SuperSpeed link 訓練失敗卡在 Inactive。\r\n");
    Print (L"  常見於線材品質差、轉接頭、或裝置本身 SS 連線有問題。\r\n");
    Print (L"  【該做什麼】[5] 會下 Warm Port Reset 嘗試重訓練。\r\n");
    return;
  }

  if (Pls == PLS_DISABLED) {
    Print (L"  【現況】有電但 link 被停用（PP=1, PLS=Disabled）\r\n");
    Print (L"  【該做什麼】[5] 會先 reset，若仍卡住會用 LWS 把 PLS 導回 RxDetect。\r\n");
    return;
  }

  if (Ccs) {
    Print (L"  【現況】正常，有裝置接著%s\r\n", IsBootPort ? L"（就是本工具的開機來源）" : L"");
    Print (L"  CCS=1 有偵測到裝置，PED=%d %s，PLS=%s。\r\n",
           Ped, Ped ? L"（port 已啟用，可以傳資料）" : L"（port 尚未啟用）",
           PlsToString (Pls));
    if (ProtoMajor == 3 && Pls == PLS_U0) {
      Print (L"  PLS=U0 代表 SuperSpeed link 正常運作中。\r\n");
    } else if (ProtoMajor == 2 && Ped) {
      Print (L"  USB2 port 已 enable，裝置枚舉完成。\r\n");
    }
    Print (L"  【該做什麼】不用動它。\r\n");
    return;
  }

  Print (L"  【現況】正常，空的 port（PP=1, CCS=0, PLS=%s）\r\n", PlsToString (Pls));
  Print (L"  有供電、沒接東西，正在等裝置插入。這是空 port 的預期狀態。\r\n");
  Print (L"  【該做什麼】不用動它。\r\n");
}

VOID
PrintPortDetail (
  IN UINTN  PortIdx
  )
{
  EFI_STATUS            Status;
  PORT_STATE            *Port = &gPorts[PortIdx];
  XHCI_CONTROLLER_INFO  *Ctrl = &gControllers[Port->ControllerIndex];
  PORT_PROTO_INFO       *Proto = &Ctrl->PortProto[Port->PortNumber];
  UINT32                Portsc;
  UINT64                Addr;
  UINTN                 FsIdx;
  BOOLEAN               HasFs;
  UINT8                 Pls, Speed;

  Status = ReadPortsc (Ctrl, Port->PortNumber, &Portsc);
  if (EFI_ERROR (Status)) {
    Print (L"\r\n  [錯誤] 讀取 PORTSC 失敗: %r\r\n", Status);
    return;
  }
  Port->LastPortsc = Portsc;
  Pls   = (UINT8)((Portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT);
  Speed = (UINT8)((Portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);

  Print (L"\r\n============================================================\r\n");
  Print (L"  Port 詳細診斷  [索引 %d]  ->  Ctrl#%d Port %d\r\n",
         PortIdx, Port->ControllerIndex, Port->PortNumber);
  Print (L"============================================================\r\n");

  // ---- (1) 這個 port 在哪裡 ----
  Print (L"\r\n(1) 這個 port 在哪裡\r\n");
  Print (L"    xHCI 控制器 : Seg%d Bus%02X Dev%02X Func%X%s\r\n",
         Ctrl->Segment, Ctrl->Bus, Ctrl->Device, Ctrl->Function,
         Ctrl->IsBootSource ? L"  <= 本工具就是從這顆開機的" : L"");
  Print (L"    HCIVERSION  : 0x%04X （xHCI %d.%d）\r\n",
         Ctrl->HciVersion, (Ctrl->HciVersion >> 8) & 0xFF, (Ctrl->HciVersion >> 4) & 0xF);

  HasFs = FindFsOnPort (Ctrl, Port->PortNumber, &FsIdx);
  if (HasFs) {
    Print (L"    檔案系統    : 這個 port 上的裝置 = FS#%d\r\n", FsIdx);
    Print (L"                  （本工具的枚舉序，一般就是 UEFI Shell 的 fs%d:）\r\n", FsIdx);
  } else if (Portsc & PORTSC_CCS) {
    Print (L"    檔案系統    : 有接裝置，但它沒有 SimpleFileSystem\r\n");
    Print (L"                  （可能是鍵盤/滑鼠/hub，或未格式化成 FAT）\r\n");
  } else {
    Print (L"    檔案系統    : 無（這個 port 沒接裝置）\r\n");
  }

  // ---- (2) PORTSC 位址是怎麼算出來的 ----
  Addr = Ctrl->OperationalBase + XHCI_PORTSC_BASE_OFFSET
         + XHCI_PORTSC_PORT_STRIDE * (Port->PortNumber - 1);

  Print (L"\r\n(2) PORTSC 的位址是怎麼算出來的\r\n");
  Print (L"    a. 讀 PCI config 0x10 的 BAR0     = 0x%08X\r\n", Ctrl->Bar0Raw);
  if (Ctrl->Bar64) {
    Print (L"       BAR0 bit2:1=10 -> 64-bit BAR，再讀 0x14 = 0x%08X\r\n", Ctrl->Bar1Raw);
  }
  Print (L"       低 4 bit 是旗標要遮掉 -> MMIO Base = 0x%LX\r\n", Ctrl->MmioBase);
  Print (L"    b. 讀 MMIO+0x00 的 CAPLENGTH      = 0x%02X (%d)\r\n",
         Ctrl->CapLength, Ctrl->CapLength);
  Print (L"       Capability 區長度由它決定，不能寫死 0x20\r\n");
  Print (L"       Operational Base = MMIO + CAPLENGTH = 0x%LX\r\n", Ctrl->OperationalBase);
  Print (L"    c. PORTSC(n) = OpBase + 0x400 + 0x10 x (n-1)\r\n");
  Print (L"                 = 0x%LX + 0x400 + 0x10 x %d\r\n",
         Ctrl->OperationalBase, Port->PortNumber - 1);
  Print (L"                 = 0x%LX   <= 本頁數值就是從這裡讀的\r\n", Addr);

  // ---- (3) MaxPorts / USB2 vs USB3 是怎麼判斷的 ----
  Print (L"\r\n(3) 這個 port 的規格是怎麼判斷出來的\r\n");
  Print (L"    HCSPARAMS1 (MMIO+0x04) = 0x%08X\r\n", Ctrl->HcsParams1);
  Print (L"      -> MaxPorts = bit31:24 = %d，所以掃 Port 1..%d\r\n",
         Ctrl->MaxPorts, Ctrl->MaxPorts);
  Print (L"    HCCPARAMS1 (MMIO+0x10) = 0x%08X\r\n", Ctrl->HccParams1);
  Print (L"      -> xECP = bit31:16 << 2 = 0x%X （Extended Capability 串鏈起點）\r\n",
         ((Ctrl->HccParams1 >> HCCPARAMS1_XECP_SHIFT) & 0xFFFF) << 2);
  if (Proto->Major != 0) {
    Print (L"    沿串鏈找 ID=0x02 (Supported Protocol)，在 offset 0x%X 找到：\r\n",
           Proto->CapOffset);
    Print (L"      Name=\"%a\"  Rev=%d.%d  PortOffset=%d  PortCount=%d\r\n",
           Proto->Name, Proto->Major, Proto->Minor, Proto->PortOffset, Proto->PortCount);
    Print (L"      -> 它宣告 Port %d..%d 都是 USB%d\r\n",
           Proto->PortOffset, Proto->PortOffset + Proto->PortCount - 1, Proto->Major);
    Print (L"      -> Port %d 落在這個範圍，所以判定為 USB%d\r\n",
           Port->PortNumber, Proto->Major);
    Print (L"    這件事很重要：USB3 用 Warm Port Reset (bit31)，\r\n");
    Print (L"    USB2 用 Port Reset (bit4)，下錯了 reset 不會生效。\r\n");
  } else {
    Print (L"    找不到涵蓋 Port %d 的 Supported Protocol cap，協定未知。\r\n",
           Port->PortNumber);
    Print (L"    復原時會保守地當成 USB3，使用 Warm Port Reset。\r\n");
  }

  PauseForKey ();

  // ---- (4) 逐位元解碼 ----
  Print (L"\r\n(4) 即時 PORTSC = 0x%08X 逐位元解碼\r\n", Portsc);
  Print (L"    bit0  CCS = %d  連線狀態      %s\r\n", (Portsc >> 0) & 1,
         (Portsc & PORTSC_CCS) ? L"有裝置" : L"沒裝置");
  Print (L"    bit1  PED = %d  port 啟用     %s\r\n", (Portsc >> 1) & 1,
         (Portsc & PORTSC_PED) ? L"已啟用" : L"未啟用");
  Print (L"    bit3  OCA = %d  過流中(唯讀)  %s\r\n", (Portsc >> 3) & 1,
         (Portsc & PORTSC_OCA) ? L"<< 過電流進行中!" : L"正常");
  Print (L"    bit4  PR  = %d  Port Reset    %s\r\n", (Portsc >> 4) & 1,
         (Portsc & PORTSC_PR) ? L"reset 進行中" : L"閒置");
  Print (L"    bit8:5PLS = 0x%X 連結狀態     %s\r\n", Pls, PlsToString (Pls));
  Print (L"    bit9  PP  = %d  Port Power    %s\r\n", (Portsc >> 9) & 1,
         (Portsc & PORTSC_PP) ? L"有供電" : L"<< 沒供電");
  Print (L"    b13:10Spd = %d  速度          %s\r\n", Speed, SpeedToString (Speed));
  Print (L"    bit17 CSC = %d  連線變更\r\n", (Portsc >> 17) & 1);
  Print (L"    bit19 WRC = %d  暖重置變更\r\n", (Portsc >> 19) & 1);
  Print (L"    bit20 OCC = %d  過流事件記錄  %s\r\n", (Portsc >> 20) & 1,
         (Portsc & PORTSC_OCC) ? L"<< 曾發生過電流" : L"無記錄");
  Print (L"    bit21 PRC = %d  重置完成變更\r\n", (Portsc >> 21) & 1);
  Print (L"    bit22 PLC = %d  連結狀態變更\r\n", (Portsc >> 22) & 1);
  Print (L"    bit24 CAS = %d  冷插入狀態\r\n", (Portsc >> 24) & 1);
  Print (L"    bit31 WPR = %d  Warm Reset\r\n", (Portsc >> 31) & 1);

  // ---- (5) 白話診斷 ----
  Print (L"\r\n(5) 這個 port 發生了什麼事\r\n");
  PrintPortDiagnosis (Portsc, Port->ProtocolMajor, Ctrl->IsBootSource && HasFs);

  Print (L"\r\n    復原重試次數：%d / %d\r\n", Port->RecoveryRetries, MAX_RECOVERY_RETRIES);
  Print (L"============================================================\r\n");
}

// ============================================================
//  UI
// ============================================================
VOID
PrintBanner (
  VOID
  )
{
  Print (L"\r\n=========================================================\r\n");
  Print (L"  UsbOcRecover v%s - xHCI 過電流(OC) 偵測與復原\r\n", USBOCREC_VERSION_STR);
  Print (L"  安全規則：OCA=1 時不上電、不 reset；寫入前雙重確認\r\n");
  Print (L"=========================================================\r\n");
}

VOID
PrintMainMenu (
  VOID
  )
{
  Print (L"\r\n--------------------------------\r\n");
  Print (L"  [1] 掃描 OC\r\n");
  Print (L"  [5] 一鍵復原\r\n");
  Print (L"  [9] Port 詳細診斷\r\n");
  Print (L"  [0] 離開\r\n");
  Print (L"--------------------------------\r\n");
  Print (L"  請選擇: ");
}

EFI_STATUS
HandleMenuInput (
  VOID
  )
{
  CHAR16  Choice;
  UINTN   Idx;

  Choice = ReadSingleKey ();
  Print (L"%c\r\n", Choice);

  switch (Choice) {
    case L'1':
      ScanForOc (TRUE);
      break;

    case L'5':
      RecoverAllOcPorts ();
      break;

    case L'9':
      Print (L"\r\n  輸入 Port 索引 (0-%d，就是 [1] 掃描結果最左邊那欄): ",
             gPortCount > 0 ? gPortCount - 1 : 0);
      Idx = ReadUintFromUser (4);
      if (Idx < gPortCount) {
        PrintPortDetail (Idx);
      } else {
        Print (L"  [錯誤] 索引超出範圍\r\n");
      }
      break;

    case L'0':
    case L'q':
    case L'Q':
      gRunning = FALSE;
      break;

    default:
      Print (L"\r\n  [無效輸入]\r\n");
      break;
  }
  return EFI_SUCCESS;
}

// ============================================================
//  Log
// ============================================================
VOID
AppLog (
  IN LOG_LEVEL     Level,
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST   Args;
  LOG_ENTRY *Entry;
  UINT64    TimeMs;

  if (gLogCount >= MAX_LOG_ENTRIES) {
    gLogCount = MAX_LOG_ENTRIES - 1;
  }
  Entry = &gLogBuffer[gLogCount];
  Entry->Level = Level;
  GetTimestampMs (&TimeMs);
  Entry->Timestamp = TimeMs;

  VA_START (Args, Format);
  UnicodeVSPrint (Entry->Message, sizeof (Entry->Message), Format, Args);
  VA_END (Args);
  gLogCount++;

  if (Level >= LOG_ERROR) {
    Print (L"  [LOG:%s] %s\r\n", Level == LOG_CRITICAL ? L"CRIT" : L"ERR ", Entry->Message);
  }
}

EFI_STATUS
ExportLogToFile (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *FsHandles;
  UINTN                            FsCount, FsIdx, Li;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root, *LogFile;
  CHAR8                            LineBuf[512];
  UINTN                            LineLen;
  BOOLEAN                          Saved = FALSE;
  STATIC CONST CHAR8  *LevelStrA[4] = { "INFO", "WARN", "ERR ", "CRIT" };
  STATIC CONST CHAR16 *LevelStrW[4] = { L"INFO", L"WARN", L"ERR ", L"CRIT" };

  Print (L"\r\n[Log] 匯出 %d 筆到 \\OCRecoverLog.txt...\r\n", gLogCount);

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &FsCount, &FsHandles);
  if (EFI_ERROR (Status)) {
    goto FallbackConsole;
  }

  for (FsIdx = 0; FsIdx < FsCount; FsIdx++) {
    Status = gBS->HandleProtocol (FsHandles[FsIdx], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = Root->Open (Root, &LogFile, L"OCRecoverLog.txt",
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR (Status)) {
      Root->Close (Root);
      continue;
    }

    {
      CONST CHAR8 *Header = "UsbOcRecover v2.0 Log\r\n=====================\r\n";
      LineLen = AsciiStrLen (Header);
      LogFile->Write (LogFile, &LineLen, (VOID *)Header);
    }
    {
      CHAR8  MsgA[MAX_LOG_LINE_LEN];
      UINTN  Mi;
      for (Li = 0; Li < gLogCount; Li++) {
        LOG_ENTRY *E = &gLogBuffer[Li];
        for (Mi = 0; Mi < MAX_LOG_LINE_LEN - 1 && E->Message[Mi] != 0; Mi++) {
          MsgA[Mi] = (E->Message[Mi] < 0x80) ? (CHAR8)E->Message[Mi] : '?';
        }
        MsgA[Mi] = '\0';
        LineLen = AsciiSPrint (LineBuf, sizeof (LineBuf), "[%Lu ms][%a] %a\r\n",
                               E->Timestamp, (E->Level < 4) ? LevelStrA[E->Level] : "????", MsgA);
        LogFile->Write (LogFile, &LineLen, LineBuf);
      }
    }
    LogFile->Close (LogFile);
    Root->Close (Root);
    Print (L"[Log] 已存到 FS#%d:\\OCRecoverLog.txt\r\n", FsIdx);
    Saved = TRUE;
    break;
  }
  FreePool (FsHandles);
  if (Saved) {
    return EFI_SUCCESS;
  }

FallbackConsole:
  Print (L"\r\n[Log] === Console Dump (%d 筆) ===\r\n", gLogCount);
  for (Li = 0; Li < gLogCount; Li++) {
    LOG_ENTRY *E = &gLogBuffer[Li];
    Print (L"[%Lu ms][%s] %s\r\n", E->Timestamp,
           (E->Level < 4) ? LevelStrW[E->Level] : L"????", E->Message);
  }
  return EFI_NOT_FOUND;
}

// ============================================================
//  輔助
// ============================================================
BOOLEAN
ConfirmAction (
  IN CONST CHAR16  *Prompt
  )
{
  CHAR16  Ch;
  Print (L"%s？(Y/N): ", Prompt);
  Ch = ReadSingleKey ();
  Print (L"%c\r\n", Ch);
  return (Ch == L'Y' || Ch == L'y');
}

BOOLEAN
DoubleConfirmAction (
  IN CONST CHAR16  *Prompt
  )
{
  if (!ConfirmAction (Prompt)) {
    return FALSE;
  }
  return ConfirmAction (L"  請再次確認，是否繼續");
}

VOID
PauseForKey (
  VOID
  )
{
  Print (L"\r\n    -- 按任意鍵看下半部 --\r\n");
  ReadSingleKey ();
}

CHAR16
ReadSingleKey (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;
  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  return Key.UnicodeChar;
}

//
// 讀入十進位整數（Enter 結束）。
//
UINTN
ReadUintFromUser (
  IN UINTN  MaxDigits
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;
  UINTN          Value = 0;
  UINTN          Count = 0;

  while (Count < MaxDigits) {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      break;
    }
    if (Key.UnicodeChar >= L'0' && Key.UnicodeChar <= L'9') {
      Value = Value * 10 + (Key.UnicodeChar - L'0');
      Count++;
      Print (L"%c", Key.UnicodeChar);
    }
  }
  Print (L"\r\n");
  return Value;
}

EFI_STATUS
GetTimestampMs (
  OUT UINT64  *Ms
  )
{
  EFI_TIME    Time;
  EFI_STATUS  Status;
  Status = gRT->GetTime (&Time, NULL);
  if (EFI_ERROR (Status)) {
    *Ms = 0;
    return Status;
  }
  *Ms = (UINT64)Time.Hour * 3600000 + (UINT64)Time.Minute * 60000
      + (UINT64)Time.Second * 1000 + (UINT64)(Time.Nanosecond / 1000000);
  return EFI_SUCCESS;
}

EFI_STATUS
WaitMs (
  IN UINTN  Ms
  )
{
  return gBS->Stall (Ms * 1000);
}
