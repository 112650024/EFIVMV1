# 怎麼寫自己的 EFI —— 從你現在的位置開始

你已經做完這些了：

```bash
sudo apt install git
git clone https://github.com/112650024/EFIVMV1.git
cd EFIVMV1
chmod +x *.sh
./setup-edk2-ubuntu.sh
```

**接下來要幹嘛，這份文件從頭講到尾。**

---

## 先搞懂一件事，不然後面都會卡住

這是最多人卡住的地方，先講清楚：

```
   你寫的 .c 檔                     編譯器                    .efi 檔
   （人看得懂的程式碼）    ──────────────────▶    （機器看得懂的成品）

   Print(L"Hello");                              4c 8d 42 ff 48 85 d2 ...

   ✅ 可以讀、可以改                            ❌ 不能讀、不能改
```

| 副檔名 | 是什麼 | 能不能編輯 |
|---|---|---|
| **`.c`** | **原始碼**，你打的字 | ✅ 就是拿來改的 |
| `.inf` | 說明書：這個模組叫什麼、進入點是哪個函式 | ✅ 偶爾改 |
| `.dsc` | 說明書：整包要連哪些函式庫 | ✅ 偶爾改 |
| **`.efi`** | **編譯完的成品** | ❌ **不能** |

**`.efi` 不是一種「格式」，是終點。** 就像烤好的蛋糕 —— 你不能把蛋糕變回麵粉和蛋。

所以：

- 「我要寫 EFI」→ **你要寫的是 `.c`**
- 「我有檔案想編譯成 EFI」→ **那個檔案要是 `.c`**
- 「我有 `.efi` 想改」→ **辦不到**（下面第 3 節說明能做什麼）

---

## 0. 先確認環境是好的

```bash
cd ~/EFIVMV1
./setup-edk2-ubuntu.sh --check
```

每一項都要是 `[OK]`，最後要看到：

```
==> 環境正常，可以直接跑 ocbuild
```

如果有 `[X]`，照它說的做。有問題就把整段貼出來問。

---

## 1. 情況 A：我要從零開始寫一個 EFI

### 1-1. 建立專案（你還沒建，這步一定要做）

```bash
cd ~/EFIVMV1
./new-efi-project.sh MyApp
```

> `MyApp` 是名字，你可以取別的（例如 `UsbTest`、`MemDump`）。
> **只能用英文字母和數字，開頭要是字母** —— 因為這個名字會直接
> 變成 C 的函式名稱和檔名。取中文的話腳本會擋你。

跑完會看到：

```
==> 專案建好了：/home/kai/MyApp

  MyAppPkg/MyApp/MyApp.c     <- 改這個（你的程式碼）
  MyAppPkg/MyApp/MyApp.inf   <- 加檔案 / 加 Protocol 時改這個
  MyAppPkg/MyAppPkg.dsc      <- 加函式庫時改這個
  build.sh                   <- 編譯
```

### 1-2. 專案長什麼樣

```
/home/kai/MyApp/                         ← 專案根目錄
├── build.sh                             ← 編譯用，打 ./build.sh
├── .gitignore
└── MyAppPkg/                            ← 「套件」
    ├── MyAppPkg.dsc                     ← 整包的設定
    └── MyApp/                           ← 「模組」
        ├── MyApp.c                      ← ★ 你的程式碼在這 ★
        └── MyApp.inf                    ← 這個模組的設定
```

為什麼要三層這麼麻煩？因為 edk2 是設計來編**整個 BIOS**的（幾百個模組），
所以有「套件 → 模組」的階層。我們只有一個模組，但架構還是得照它的規矩。

**你 99% 的時間只會碰 `MyApp.c` 這一個檔案。**

### 1-3. 先編一次，確認整條路是通的

**還沒改任何東西就先編。** 這樣如果之後壞了，你知道是自己改壞的。

```bash
cd ~/MyApp
./build.sh
```

要看到：

```
================ 編譯成功 ================
  /home/kai/MyApp/MyApp.efi
  大小 : 6336 bytes
  類型 : PE32+ executable (EFI application) x86-64
```

**看到這個就代表整條路通了。** 現在才開始改。

### 1-4. 怎麼打開檔案來改

三種方法，挑一種：

**方法 1：圖形介面（最簡單）**

打開「檔案」→ 家目錄 → `MyApp` → `MyAppPkg` → `MyApp` →
在 `MyApp.c` 上按右鍵 → 用文字編輯器開啟。

或直接下指令開：

```bash
gedit ~/MyApp/MyAppPkg/MyApp/MyApp.c
```

> `gedit` 是 Ubuntu 22.04 內建的記事本。如果說找不到：
> `sudo apt install -y gedit`

**方法 2：終端機裡改（不用離開終端機）**

```bash
nano ~/MyApp/MyAppPkg/MyApp/MyApp.c
```

`nano` 的操作：直接打字就能改，`Ctrl+O` 存檔（然後按 Enter），`Ctrl+X` 離開。
畫面最下面有提示，`^O` 就是 `Ctrl+O` 的意思。

**方法 3：VS Code（有的話最舒服）**

```bash
code ~/MyApp
```

### 1-5. 產生出來的程式碼長這樣

```c
/** @file
  MyApp.c - my UEFI application
**/

#include <Uefi.h>                                  // ← 基本型別（UINT32、EFI_STATUS…）
#include <Library/UefiLib.h>                       // ← Print() 在這裡
#include <Library/UefiApplicationEntryPoint.h>     // ← 程式進入點
#include <Library/UefiBootServicesTableLib.h>      // ← gBS、gST

EFI_STATUS
EFIAPI
MyAppMain (                                        // ← ★ 程式從這裡開始跑 ★
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN          Index;
  EFI_INPUT_KEY  Key;

  Print (L"\r\n");
  Print (L"==============================\r\n");
  Print (L"  MyApp\r\n");
  Print (L"==============================\r\n");
  Print (L"\r\n");
  Print (L"UEFI Version : %d.%d\r\n",
         SystemTable->Hdr.Revision >> 16,
         SystemTable->Hdr.Revision & 0xFFFF);
  Print (L"Firmware     : %s\r\n", SystemTable->FirmwareVendor);

  Print (L"\r\nPress any key to exit...\r\n");
  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);   // ← 等你按鍵
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);

  return EFI_SUCCESS;                              // ← 回報「成功」
}
```

**幾個跟一般 C 不一樣、會讓你踩到的地方：**

| 寫法 | 為什麼 |
|---|---|
| `Print()` 不是 `printf()` | UEFI 沒有 C 標準函式庫。沒有 `stdio.h`、沒有 `malloc` |
| 字串前面要加 `L`：`L"Hello"` | UEFI 用的是 16-bit 寬字元（CHAR16），不是一般的 char |
| 換行要 `\r\n` 不是 `\n` | UEFI 主控台需要兩個都給，只給 `\n` 游標不會回到最左邊 |
| 進入點叫 `MyAppMain` 不是 `main` | 名字是 `.inf` 裡的 `ENTRY_POINT` 指定的 |

### 1-6. 改一行試試看

把這行：

```c
  Print (L"  MyApp\r\n");
```

改成：

```c
  Print (L"  Hello from Kai!\r\n");
  Print (L"  1 + 2 = %d\r\n", 1 + 2);
```

存檔，然後：

```bash
cd ~/MyApp
./build.sh
```

驗證你改的東西真的進去了：

```bash
strings -el ~/MyApp/MyApp.efi | grep "Hello from Kai"
```

有印出來就對了。**這就是完整的開發循環：改 `.c` → `./build.sh` → 檢查。**

---

## 2. 情況 B：我已經有 `.c` 檔了

### 2-1. 只有一個 `.c` 檔

```bash
cd ~/EFIVMV1
./new-efi-project.sh MyApp          # 一樣先建專案
```

然後把你的程式碼**貼進** `~/MyApp/MyAppPkg/MyApp/MyApp.c`（把原本的內容整個蓋掉）。

**但是有一件事一定要改：** `.inf` 裡的 `ENTRY_POINT` 必須跟你的**進入點函式名**一樣。

打開 `~/MyApp/MyAppPkg/MyApp/MyApp.inf`，看這行：

```
  ENTRY_POINT    = MyAppMain
```

如果你的 `.c` 裡的進入點函式叫 `UefiMain`，就要改成：

```
  ENTRY_POINT    = UefiMain
```

**兩邊名字對不起來 = 連結錯誤**，會看到類似 `undefined reference to MyAppMain`。

### 2-2. 直接用檔案覆蓋

如果你的 `.c` 在別的地方（例如共用資料夾）：

```bash
cp /media/sf_共用資料夾/我的程式.c ~/MyApp/MyAppPkg/MyApp/MyApp.c
cd ~/MyApp
./build.sh
```

### 2-3. 有好幾個 `.c` 檔

把它們**全部**丟進 `~/MyApp/MyAppPkg/MyApp/`：

```bash
cp /media/sf_共用資料夾/*.c ~/MyApp/MyAppPkg/MyApp/
cp /media/sf_共用資料夾/*.h ~/MyApp/MyAppPkg/MyApp/
```

然後編輯 `MyApp.inf`，在 `[Sources]` 底下**每個 `.c` 列一行**（`.h` 不用列）：

```
[Sources]
  MyApp.c
  Helper.c
  Parser.c
```

> **漏列的話**會看到 `undefined reference to XXX` —— 那個 `.c` 根本沒被編譯。

### 2-4. 程式碼用到 `.inf` 裡沒宣告的東西

例如你的程式要用 PCI：

```c
#include <Protocol/PciIo.h>
...
gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, ...);
```

`.inf` 要加 `[Protocols]` 區塊：

```
[Protocols]
  gEfiPciIoProtocolGuid                 ## CONSUMES
```

要用檔案系統就再加：

```
[Protocols]
  gEfiPciIoProtocolGuid                 ## CONSUMES
  gEfiSimpleFileSystemProtocolGuid      ## CONSUMES
  gEfiLoadedImageProtocolGuid           ## CONSUMES
```

**不加的話**：編譯會過（`.h` 找得到），但**連結時**會噴
`undefined reference to gEfiPciIoProtocolGuid`。

同理，用到新的函式庫（例如 `MemoryAllocationLib` 的 `AllocatePool`），
`.inf` 的 `[LibraryClasses]` 要加：

```
[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib
  UefiBootServicesTableLib
  MemoryAllocationLib          ← 新加的
```

> **不知道要加什麼？** 直接看 `~/EFIVMV1/UsbOcRecoverPkg/UsbOcRecover/UsbOcRecover.inf`。
> 那是一個真的在用 PCI、檔案系統、DevicePath 的完整範例，照抄就對了。

### 2-5. 我的 `.c` 有中文註解

**存檔一定要選「UTF-8 with BOM」。**

- 在 Linux 用 GCC 編：沒 BOM 也沒事
- **在 Windows 用 MSVC 編：沒 BOM 會爆**，噴 `error C2001: newline in constant`，
  而且錯誤會指在一行**完全正常**的程式碼上，讓你找到瘋掉

VS Code：右下角點編碼 → `Save with Encoding` → `UTF-8 with BOM`。

**`.inf` 和 `.dsc` 則是完全不能有中文**（連註解都不行），
否則會噴 `error 0004: File read failure`，訊息完全沒有幫助。

---

## 3. 情況 C：我有一個 `.efi` 檔

### 3-1. 想「編輯」它 → 辦不到

不是工具不夠好，是**編譯本身就是單向且有損的**。
變數名、函式名、註解、型別 —— 在編譯那一刻就永久消失了。

**就算把 `.efi` 給 AI 也一樣挖不回原始碼。** 資訊已經不存在了，
不是「還沒找到」，是「不在裡面」。

實際示範。這是 `HelloWorld` 的原始碼：

```c
Print (L"UEFI Hello World!\n");
```

這是從它編出來的 `.efi` 挖出來的東西：

```asm
240: 31 c0        xor    eax,eax
242: 48 85 c9     test   rcx,rcx
245: 74 1a        je     0x261
250: 80 3c 01 00  cmp    BYTE PTR [rcx+rax*1],0x0
```

連 `Print` 這個名字都不見了，只剩一個算字串長度的迴圈。

### 3-2. 但你可以做這些

```bash
# 看它是什麼東西
file 檔案.efi

# 撈出裡面的文字（意外地有用）
strings -el 檔案.efi

# 反組譯 —— 看組合語言，不是 C
objdump -d -M intel 檔案.efi | less
```

`strings -el` 那個 `-el` 是關鍵：EFI 的字串是 UTF-16LE，
不加 `-el` 只會撈到亂碼。

### 3-3. 真的要逆向

**Ghidra**（NSA 出的，免費）能產出**類 C 的偽代碼**：

```bash
sudo apt install -y openjdk-17-jdk
# 然後去 https://ghidra-sre.org/ 下載
```

看得懂邏輯，但變數會叫 `local_18`、`uVar3`，註解永遠不會回來。
那是「猜出來的程式碼」，不是原始碼。

### 3-4. 你真正想要的可能是這個

如果你有一個 `.efi`，想要「一個做一樣事情的程式」——
**不要走逆向，直接重寫比較快。**

跟我說那個 `.efi` **應該做什麼**（例如「列出所有 USB port 的狀態」），
我幫你寫 `.c`。這比從組合語言猜他當初怎麼寫的快十倍，而且你會有真正的原始碼。

---

## 4. 情況 D：我有別人的完整 edk2 專案

先看那個資料夾裡有什麼：

| 看到 | 那是 | 怎麼辦 |
|---|---|---|
| 有 `.inf` **和** `.dsc` | 完整 edk2 專案 | 抄 `~/MyApp/build.sh`，改成它的名字 |
| 只有 `.inf` 沒 `.dsc` | 單一 edk2 模組 | 用 `new-efi-project.sh` 建一個，把它的 `.inf` 加進 `.dsc` 的 `[Components]` |
| 有 `Makefile`、`#include <efi.h>` | **gnu-efi** 專案，不是 edk2 | 不同工具鏈：`sudo apt install gnu-efi`，然後 `make` |
| 只有 `.c` | 裸原始碼 | 看情況 B |

---

## 5. 編譯失敗怎麼看

編譯失敗會看到 `exit=1` 和：

```
[失敗] 編譯失敗，看上面的錯誤訊息。
```

**往上找第一個 `error:`。** 它會告訴你檔名、行號、欄號：

```
/home/kai/MyApp/MyAppPkg/MyApp/MyApp.c:37:1: error: expected ‘;’ before ‘}’ token
                                        ^^ ^  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                                      行號 欄號        錯在哪
```

**永遠看最上面那個 error，不要看最後一個。** 第一個錯常常會引發後面
一連串假的錯誤，修好第一個，後面可能全部消失。

### 常見錯誤對照表

| 錯誤訊息 | 意思 | 怎麼修 |
|---|---|---|
| `expected ';' before ...` | 少了分號 | 看那一行**和上一行** |
| `undefined reference to 'XXXMain'` | `.inf` 的 `ENTRY_POINT` 跟你的函式名對不起來 | 改 `.inf` 的 `ENTRY_POINT` |
| `undefined reference to 'gEfiXxxProtocolGuid'` | `.inf` 的 `[Protocols]` 沒宣告 | 加進 `[Protocols]` |
| `undefined reference to '你的函式'` | 那個 `.c` 沒列進 `[Sources]` | 加進 `[Sources]` |
| `XXX.h: No such file or directory` | `.inf` 的 `[Packages]` 或 `[LibraryClasses]` 少東西 | 加對應的 LibraryClass |
| `error 0004: File read failure` | `.inf`/`.dsc` 有中文 | 拿掉，只能純 ASCII |
| `error C2001: newline in constant`（只有 Windows） | `.c` 有中文但沒 BOM | 存成 UTF-8 with BOM |
| `warning: ... [-Werror=...]` | 有警告，而 edk2 把警告當錯誤 | 照它說的修，不能忽略 |

> edk2 用 `-Werror`，**任何警告都是錯誤**。連「變數宣告了沒用到」都會讓你編不過。
> 這很煩，但也逼你把程式寫乾淨。

### 卡住了怎麼辦

```bash
./build.sh > /tmp/err.log 2>&1     # 把完整輸出存起來
```

把 `/tmp/err.log` 裡**第一個 error 前後 10 行**貼給我。

---

## 6. 編好的 `.efi` 怎麼用

### 6-1. 放到 USB 隨身碟（實機執行）

隨身碟要格式化成 **FAT32**（NTFS / exFAT 都不行，UEFI 韌體不認）：

```
隨身碟根目錄/
└── EFI/
    └── BOOT/
        └── BOOTX64.EFI      ← 你的 MyApp.efi 改成這個名字
```

插上去 → 開機按 F12（或你主機板的開機選單鍵）→ 選那隻隨身碟 → 直接跑。

### 6-2. 在 VM 裡先試跑（不用碰實機）

```bash
sudo apt install -y qemu-system-x86 ovmf
mkdir -p /tmp/esp/EFI/BOOT
cp ~/MyApp/MyApp.efi /tmp/esp/EFI/BOOT/BOOTX64.EFI
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
                   -drive format=raw,file=fat:rw:/tmp/esp -m 512
```

會開一個視窗，跑你的程式。**改完馬上就能看到結果，不用一直重開機。**

> 但 QEMU 裡看不到真實硬體狀態（沒有真的 USB 過流保護、
> PCI 裝置也是假的）。邏輯驗證可以，硬體相關的還是得上實機。

---

## 7. 建議：用 git 管你的專案

```bash
cd ~/MyApp
git init
git add -A
git commit -m "first version"
```

之後改壞了：

```bash
git diff              # 看我改了什麼
git checkout .        # 全部還原到上次 commit
```

**改到一半編不過又不知道自己動了什麼**的時候，這會救你一命。

---

## 常用指令總表

```bash
# 環境
cd ~/EFIVMV1
./setup-edk2-ubuntu.sh --check      # 環境診斷

# 開新專案
./new-efi-project.sh MyApp          # 建在 ~/MyApp

# 日常
cd ~/MyApp
gedit MyAppPkg/MyApp/MyApp.c        # 改程式碼
./build.sh                          # 編譯（RELEASE）
./build.sh DEBUG                    # 編譯（DEBUG）

# 檢查
file ~/MyApp/MyApp.efi              # 這是什麼
strings -el ~/MyApp/MyApp.efi       # 撈裡面的字串
```

---

## 還是卡住的話

把這三樣貼給我：

1. 你**想做什麼**
2. 你**打了什麼指令**
3. **完整的錯誤訊息**（第一個 `error:` 前後 10 行）

或者你只是想知道「怎麼用 C 做某件事」（讀 PCI、列 USB、讀檔案…），
直接說要做什麼，我幫你寫 `.c`。
