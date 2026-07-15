# EFIVMV1 — 在 Ubuntu 裡編譯 UsbOcRecover.efi

xHCI USB 過電流(OC) 復原工具的原始碼 + 一鍵建置環境腳本。

**這包是要在 Ubuntu 裡 `git clone` 的。**
如果你還在找「VM 要裝哪個版本、ISO 去哪抓」，那是另一包：
[EFIUBV1](https://github.com/112650024/EFIUBV1)

---

## TL;DR

在 Ubuntu 22.04 裡：

```bash
git clone https://github.com/112650024/EFIVMV1.git
cd EFIVMV1
chmod +x *.sh
./setup-edk2-ubuntu.sh
```

跑完就會有 `UsbOcRecover.efi`。之後要重編，任何目錄打 `ocbuild` 就好。

---

## 環境需求

| 項目 | 需求 |
|---|---|
| 作業系統 | **Ubuntu 22.04 LTS**（強烈建議，原因見下） |
| 架構 | x86_64 |
| 磁碟 | 15 GB 以上可用空間 |
| 網路 | 要能連外（`apt` + `git clone` edk2 約 300 MB） |

### 為什麼一定要 22.04，不要 24.04+

**關鍵是 Python。** edk2 的 BaseTools 有些地方會 `import distutils`，
而 Python 3.12（24.04 內建）已經把 `distutils` 從標準函式庫移除了。
22.04 是 Python 3.10，`distutils` 還在，`python3-distutils` 套件也裝得到。

次要原因是 GCC。22.04 是 GCC 11，在 edk2 長期測試範圍內；
24.04 的 GCC 13 會對 edk2 多噴新 warning，而 edk2 預設 `-Werror`，
warning 就是 error。

腳本在 24.04 上也做了處理（裝不到 `python3-distutils` 就跳過），
可以試，失敗就退回 22.04。

---

## 這包有什麼

```
EFIVMV1/
├── README.md
├── setup-edk2-ubuntu.sh      環境一鍵安裝（跑一次）
├── build-efi.sh              編譯（裝好後打 ocbuild 即可）
├── verify-efi.sh             驗證 .efi 真的是這份原始碼編的
└── UsbOcRecoverPkg/          edk2 專案
    ├── UsbOcRecoverPkg.dsc
    └── UsbOcRecover/
        ├── UsbOcRecover.c
        └── UsbOcRecover.inf
```

---

## `setup-edk2-ubuntu.sh` 做了什麼

7 個步驟，全自動：

| 步驟 | 內容 |
|---|---|
| 1/7 | 檢查環境（Ubuntu 版本、磁碟、架構、sudo） |
| 2/7 | `apt install` gcc / nasm / iasl / python3 / uuid-dev … |
| 3/7 | `git clone` edk2 到 `~/edk2`，切到 `edk2-stable202411` |
| 4/7 | `git submodule update --init --recursive` |
| 5/7 | `make -C BaseTools` |
| 6/7 | 裝 `ocbuild` 指令 + 設定 PATH |
| 7/7 | 直接編一次當驗證 |

**可以重複執行** —— 已經裝好的步驟會自動跳過。

跑完之後，這個終端機要先跑一次：

```bash
source ~/.bashrc
```

（新開的終端機不用，會自動載入。）

---

## 日常使用

```bash
ocbuild                 # RELEASE 版
ocbuild DEBUG           # DEBUG 版
ocbuild --clean         # 清乾淨重編

./setup-edk2-ubuntu.sh --check    # 環境診斷，會告訴你哪一步壞了
./verify-efi.sh                   # 驗證編出來的 .efi
```

`ocbuild` 在任何目錄都能用，不用先 `cd` 回來，也不用手動 `source edksetup.sh`。

編出來的檔案：

```
~/ocbuild-work/Build/UsbOcRecoverPkg/<TARGET>_GCC5/X64/UsbOcRecover.efi
```

同時也會複製一份回這個 repo 的根目錄。

---

## 把 .efi 放上隨身碟

隨身碟要格式化成 **FAT32**（不能是 NTFS/exFAT，UEFI 韌體不認）：

```
隨身碟根目錄/
└── EFI/
    └── BOOT/
        └── BOOTX64.EFI      <- 就是 UsbOcRecover.efi 改名
```

插上去、開機選這隻隨身碟，就會直接進工具。

---

## 遇到狀況怎麼辦

**第一件事永遠是跑診斷：**

```bash
./setup-edk2-ubuntu.sh --check
```

它會逐項檢查（套件、edk2、submodule、BaseTools、ocbuild、磁碟）
並告訴你哪一項壞了、怎麼修。

### `build: command not found`

edk2 的 `build` **不是 apt 裝得到的指令**。它需要兩件事都完成：

1. `make -C ~/edk2/BaseTools` 把它編出來
2. `source ~/edk2/edksetup.sh` 把路徑匯出到 `PATH`

很多教學跳過第 1 步。用 `ocbuild` 就不用管這些，它內部都處理好了。

### `edksetup.sh` 一定要用 source，不能直接執行

```bash
./edksetup.sh          # ❌ 環境變數只存在子行程，腳本結束就沒了
source ./edksetup.sh   # ✅
```

而且**每開一個新終端機都要重來一次**。這正是 `ocbuild` 存在的理由。

### BaseTools 編到一半找不到標頭檔

submodule 沒抓：

```bash
git -C ~/edk2 submodule update --init --recursive
```

### ★ build 說成功，但 .efi 是舊版（最陰險）

`build` 回報 SUCCESS、`.efi` 也生出來、零錯誤零警告 —— 但編的是舊原始碼。

原因：`build -p UsbOcRecoverPkg/...dsc` 是**相對路徑**，
edk2 **先用 `WORKSPACE` 解析**，`PACKAGES_PATH` 只是後備。
如果 `~/edk2/` 底下剛好也有一份同名的 `UsbOcRecoverPkg`
（手動編過的人常有），它會安靜地編那份。

`build-efi.sh` 已經處理了（把 `WORKSPACE` 指到工作目錄），
而且**每次編完會自動驗證產物**，驗不過直接擋下：

```
產物驗證：通過（確認是這份原始碼的 v2.0）
```

要手動驗：

```bash
./verify-efi.sh 路徑/xxx.efi
```

### 磁碟空間不足

edk2 + submodule + Build 目錄實測約 **1.5 GB**，
但 Ubuntu Desktop 本體就要 ~12 GB。虛擬硬碟至少給 40 GB。

### 從 Windows 複製過來的 .sh 執行失敗

```
bash: ./setup-edk2-ubuntu.sh: /usr/bin/env^M: bad interpreter
```

換行變成 CRLF 了：

```bash
sudo apt install -y dos2unix && dos2unix *.sh
```

（用 `git clone` 就不會有這問題。）

---

## 改程式碼要注意的兩件事

1. **`UsbOcRecover.c` 開頭的 UTF-8 BOM 不能拿掉。**
   MSVC 沒 BOM 就用系統 codepage 讀原始檔，中文註解會被誤解，
   噴出 `error C2001: newline in constant` 而且指在一行完全正常的程式碼上。
   GCC 會自動跳過 BOM，所以留著兩邊都能編。
   VS Code 要選「UTF-8 with BOM」。

2. **`.dsc` / `.inf` 一律純 ASCII，不要寫中文。**
   edk2 的 `build.py` 用系統偏好編碼讀它們，非 UTF-8 的 locale
   （例如繁中 Windows 的 cp950）會解碼失敗，
   錯誤訊息只給你極度沒用的 `error 0004: File read failure`。

---

## 安全警告

這個工具**直接寫入 xHCI 的 MMIO 暫存器**，會影響 USB 供電狀態。

內建的安全規則（是刻意拒絕，不是壞掉）：

| 情況 | 行為 |
|---|---|
| `OCA=1`（過流進行中） | 拒絕上電與 reset —— 過流源還在，硬上電只會再燒一次。**先拔掉裝置** |
| HCRST 目標是開機來源 | 拒絕 —— 會把工具自己或開機碟砍掉 |
| 同一 port 復原 3 次 | 停止重試 —— 一直救不回代表是硬體問題 |

所有寫入 PORTSC 前都會**要求確認兩次**。

---

## 授權

BSD-2-Clause-Patent（同 edk2）。
