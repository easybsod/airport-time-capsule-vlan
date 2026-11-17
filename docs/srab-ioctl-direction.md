# SRAB ABI、暫存器與 VLAN Table

本文件的 Apple ABI 及配置值來自本地 A1470／7.9.1 源碼與實測。Linux B53 只用來交叉核對同家族的 register／bit 定義，不代表 AirPort 採用 Linux 驅動或 ioctl。

通用profile的設計見[配置流程](configuration-guide.md)，數值計算見[規劃器](../scripts/plan-vlan-profile.py)。本頁的ABI、selector、逐埠位址與位圖公式可以重用；出現VID18／1002／1003及P2目標值的表格與apply順序，則是[固定實測案例](examples/validated-three-vlan.md)的對照。

## 裝置及 ioctl 方向

使用者態程式經 `/dev/bcmsrab0` 操作 Broadcom SRAB。原廠 `esconfig` 與核心 `bcmsrab_ioctl` 的離線分析支持以下 ABI：

| ioctl | 值 | 用途 |
|---|---:|---|
| Acquire | `0x20006D00` | 取得操作鎖 |
| Release | `0x20006D01` | 釋放操作鎖 |
| READ | `0xC0106D02` | `IOC_INOUT`，帶入 selector 並寫回 value |
| WRITE | `0x80106D03` | `IOC_IN`，帶入 selector／value 寫硬體 |

NetBSD 的 IN／OUT 是相對核心的參數拷貝方向。READ 要先帶入地址，再由核心回傳值，因此是 INOUT；不能只看「參數進入核心」就把 WRITE 認成 READ。方向、參數長度與 command group 的編碼可參考 [NetBSD 6 ioccom.h](https://raw.githubusercontent.com/NetBSD/src/netbsd-6/sys/sys/ioccom.h)。

初期研究曾將 `0x80106D03` 誤當 READ，對 Switch Mode 寫入零，關掉 Forwarding Enable 而失去 SSH。那些舊輸出的零是輸入值，不是硬體讀值。本包保留的是更正後源碼。

即使 `open("/dev/bcmsrab0", O_RDONLY)`，這個 Apple driver 仍能接受 WRITE ioctl；fd 開啟旗標不是防寫界線。防寫要由工具是否有 WRITE 呼叫路徑決定。

## Request 結構與 selector

```c
struct request {
    uint32_t selector;  /* offset 0 */
    uint32_t reserved;  /* offset 4，清零 */
    uint64_t value;     /* offset 8 */
};                     /* sizeof == 16 */
```

C 程式用編譯期斷言固定長度16及 `value` 偏移8。每次 `memset` 清零，再填 selector／value，不能使用 Linux 主機自訂的 packed layout 代替。

```text
selector = (page << 24) | (register << 16) | width_code
```

| 寬度 | code | 例子 |
|---:|---:|---|
| 8 bits | 0 | `00:0B/8` → `0x000B0000` |
| 16 bits | 1 | `34:14/16` → `0x34140001` |
| 32 bits | 2 | `05:83/32` → `0x05830002` |
| 48 bits | 3 | 工具支援的 register width |
| 64 bits | 4 | MIB octet counters |

基本生命週期是 open → Acquire → 核對基線／操作 → Release → close，並檢查 ioctl、解鎖及關閉結果。鎖不是跨多個 register 的原子提交；獨立回復程序仍有必要。

## 通用逐埠位址與案例暫存器

任意納入本設備研究範圍的埠p，其PVID為 `34:(0x10+2*p)/16`，Mask為 `31:(2*p)/16`。例如P0 PVID selector=`0x34100001`、P1 Mask=`0x31020001`；不需要將新Access固定在P2。VTA的Index是目標VID，不是埠號。

以下原廠／最終值專指原固定案例；「保留」表示該引擎不改寫，只有標記「是」才會核對。新profile應依自己的before／after及所有受影響埠產生核對清單。全數讀回也不等於驗證所有交換器狀態。

| 項目 | Page:Reg／寬度 | selector | 原廠 | 最終 | 最終核對 |
|---|---|---:|---:|---:|---|
| Switch Mode | `00:0B/8` | `0x000B0000` | `0x07` | 保留 | 是 |
| VLAN Control0 | `34:00/8` | `0x34000000` | `0xE3` | 保留 | 是 |
| VLAN Control1 | `34:01/8` | `0x34010000` | `0x02` | 保留 | 否 |
| VLAN Control4 | `34:05/8` | `0x34050000` | `0xC4` | `0x40` | 是 |
| VLAN Control5 | `34:06/8` | `0x34060000` | `0x10` | `0x18` | 是 |
| P2 PVID | `34:14/16` | `0x34140001` | `18` | `1002` | 是 |
| P2 Port VLAN Mask | `31:04/16` | `0x31040001` | `0x010F` | `0x000C` | 是 |
| P3 PVID | `34:16/16` | `0x34160001` | `18` | 保留 | 是 |
| P3 Port VLAN Mask | `31:06/16` | `0x31060001` | `0x010F` | 保留 | 是 |
| P8 PVID | `34:20/16` | `0x34200001` | `18` | 保留 | 是 |
| P8 Port VLAN Mask | `31:10/16` | `0x31100001` | `0x010F` | 保留 | 是 |
| VTA Access | `05:80/8` | `0x05800000` | 動態狀態 | READ／WRITE 命令 | 是 |
| VTA Index | `05:81/16` | `0x05810001` | VID selector | VID18/1002/1003 | 每次核對 |
| VTA Entry | `05:83/32` | `0x05830002` | 查詢 latch | 下表 | 逐 VID 核對 |

其他原廠讀值：P0/P1 PVID18、Mask `0x010F`；P5 PVID1、Mask `0x01FF`；TPID `34:30/16=0x9100`；ISP Portmap `34:32/16=0`。這些不是最終引擎的寫入或完整驗證集合。

PVID register 一般位址為 `34:(0x10+2*p)`，Port VLAN Mask 為 `31:(2*p)`，兩者皆16-bit。`0x00C` 是包含 P2/P3 的埠位圖，不是 VLAN ID12，也不是允許的 VID 清單。

## Control4 與 Control5 的變化

下面的 Control4 bits7:6 與 Control5 bit3 可由 [Linux B53 register definitions](https://github.com/torvalds/linux/blob/v6.12/drivers/net/dsa/b53/b53_regs.h) 交叉核對。DT bit2 的判讀則來自本地研究的模式切換及鏡像校準，該版 Linux header 並未定義此位元；相關觀測與限制見[驗證文件](validation.md)。

| 欄位 | 原廠 `0xC4`／`0x10` | 最終 `0x40`／`0x18` |
|---|---|---|
| Control4 bit2，DT enable | 1 | 0 |
| Control4 bits7:6，Ingress VID violation policy | 3：轉送 IMP | 1：Drop |
| Control5 bit3，VLAN Table Miss Drop | 0 | 1 |

最終結果需要同時建立1003 Entry，否則開啟 table miss drop 會阻斷原有 Guest VLAN。來源最早停 DT 的中間值為 `0xC0`，仍保留 violation-to-IMP，不能提供相同的隔離條件。引擎僅為回復已知歷史配置而接受此值；`apply()` 寫入的是 `0x40`。

Control0 `0xE3` 與 Switch Mode `0x07` 保留已知原廠狀態；本整理不推定所有未核實位元均可照 Linux 家族定義修改。Service TPID 仍可讀為 `0x9100`，不代表標準模式的 WAN Customer Tag 變為 `0x9100`。

## VLAN Entry 的計算

本地原廠範本、VTA 實測與工具一致採用：

```text
members = entry & 0x1FF
untag   = (entry >> 9) & 0x1FF
extra   = entry & ~0x3FFFF

entry = preserved_extra | members | (untag << 9)
```

member 與 untag 各9 bits，以 ASIC port number 作位元位置。每個 Untag 出口應同時在 members 中。這台設備沿用額外高位 `0x00200000`（bit21）；**尚未確證其含義，不能將其命名為 VALID bit，也不能推定其他型號可照抄。**

| VID／狀態 | Members | Untag | Extra | Entry |
|---|---:|---:|---:|---:|
| 18 原廠 | `0x10F`：P0/P1/P2/P3/P8 | `0x10F` | `0x00200000` | `0x00221F0F` |
| 18 最終 | `0x10B`：P0/P1/P3/P8 | `0x10B` | `0x00200000` | `0x0022170B` |
| 1002 最終 | `0x00C`：P2/P3 | `0x004`：P2 | `0x00200000` | `0x0020080C` |
| 1003 最終 | `0x108`：P3/P8 | `0x000` | `0x00200000` | `0x00200108` |
| 1002／1003 原廠 | `0x000` | `0x000` | `0` | `0x00000000` |

例如1002：`0x00200000 | 0x000C | (0x0004 << 9) = 0x0020080C`。18從原廠排除 P2 時，member bit2 與 untag bit(9+2) 必須一起清除。

原廠1003 Entry 為零，卻能運作 Guest，是原廠 DT／違規轉送策略下的觀察；不能推論標準嚴格模式也可省略1003 Entry。

## VTA 查詢與寫入

晶片家族的命令定義可核對 [Linux B53 table access 實作](https://github.com/torvalds/linux/blob/v6.12/drivers/net/dsa/b53/b53_common.c)。本包固定引擎的真實序列如下。

查詢一個 VID：

1. WRITE Index=`vid`，再 READ Index 確認相符。
2. WRITE Access=`0x81`（START bit7 + READ opcode1）。
3. READ Access 輪詢至 bit7 清零，且最後值恰為 `0x01`。
4. READ Entry，取得該 VID 的實際內容。

寫入一個 Entry：

1. 檢查 VID／value 在固定白名單內。
2. WRITE Index，READ 回確認。
3. WRITE Entry，再 WRITE Access=`0x80`（START + WRITE opcode0）。
4. 輪詢至 bit7 清零，最後值須為 `0x00`。
5. 再跑完整查詢，確保讀回 Entry 與目標相同。

`0x82` 是整表 CLEAR 命令，最終引擎沒有此執行路徑。輪詢上限是100次 register read，沒有把它包裝成毫秒期限；阻塞 ioctl 仍可能卡住，因此外層另有程序期限。

必須分清三種工具：

| 工具／動作 | 是否送 SRAB WRITE | 是否改 VLAN Entry |
|---|---|---|
| `airport-switchctl` 的 register read | 否 | 否 |
| `airport-vlan-table-reader` | 是：Index／READ 命令 | 否 |
| 最終引擎 `verify`／`baseline` | 是：Index／READ 命令 | 否 |
| 最終引擎 `apply`／`restore` | 是 | 是，亦改 Port／Control |

直接讀 Entry latch 只看到上次查詢留下的資料，不能代表任意 VID 的內容。獨立 Table Reader 要求 Index 原先就是18，查詢後寫回18。它只核對 Switch Mode、Control0、Index、P3 PVID／Mask 及 VTA scratch 狀態；VID18 結果允許原廠或排除P2後的值，沒有完整檢查 Control4/5 或P2，不能當作整套配置驗證。最終引擎則以 VID18 查詢結束，Index 最後留18，不回復任意進入值。

## 固定案例引擎的 Apply、Restore 與限制

`apply()` 先呼叫 `verify(fd,0)` 確認已知原廠基線，依序執行：

```text
1003 Entry → 1002 Entry → 18 Entry
→ P2 Mask → P2 PVID → Control5 → Control4 → 全項 Read-back
```

先建立 Guest Entry，再切嚴格策略，可避免最終策略下 Guest 缺表。這仍是多步 register write，沒有無中斷的原子切換保證；任一步失敗就嘗試 `restore()`，且由外層回復計時器兜底。

`restore()` 先確認被修改欄位都屬於原廠／目標／已知中間值，未知狀態拒絕覆寫。接著依序回復 Control4=`0xC4`、Control5=`0x10`、P2 PVID18、P2 Mask=`0x010F`、原生18 Entry、1002/1003 Entry為零，最後驗證基線。這是回復本方案的已知基線，不是任意配置的通用 snapshot restore；也不會還原原廠 PF、Wi-Fi 或所有硬體 register。

`main()` 另核對 Switch Mode、Control0，接受的 Control5 僅為已知兩值，整段持有 SRAB lock。它不提供 arbitrary register／VID／value CLI。`wr()` 是共用底層寫函式；固定值約束來自呼叫點及上層白名單，不能只靠底層函式註解判斷。

原檔 `wr()` 上方仍有「只用 INDEX、ENTRY、ACCESS」的過時註解；目前 `set_dt()`、`set_miss_policy()`、`write_port2()` 也會呼叫它。本包為便於與來源逐位元比對而保留原檔，這裡以實際呼叫路徑勘誤。

## 埠映射與診斷依據

P2=LAN3 的直接證據是拔除／插回 LAN3 後 `01:00/16` Link Status 在 `0x012C` 與 `0x0128` 間變化，唯一差異為 bit2。P3=WAN、P8=CPU0 另有原廠 split profile、MIB 與收發路徑證據，但未分別完成 WAN 實體拔線驗證及所有內部接線解析。P0/P1 的個別實體順序未測定。

MIB 可使用 page=`0x20+port`，TxOctets=`00/64`、RxOctets=`50/64`。本包 READ 工具有固定清單的 P2/P3、P5/P8 稽核，不發送 MIB CLEAR。MIB 證明流量活動，不能單獨證明特定 VLAN 的封裝或隔離。

原映像有 `bcmsrab_ioctl`、`bcm53xx_srab_read_4/read_8`、`bcm53xx_srab_write_4/write_8` 等符號。可用附帶的離線 ELF 工具自行核對；記憶體地址隨映像而變，本實作不使用固定核心地址或記憶體 patch。
