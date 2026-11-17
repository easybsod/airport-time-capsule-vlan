# 通用配置的建置、部署與驗收

適用於 **Apple AirPort Time Capsule（2013 款，A1470），韌體 7.9.1** 的研究方法。部署前須已取得 SSH 存取權限及所需系統管理權限；本專案不提供如何開啟 SSH 的方法。

本頁說明從任意目標profile走到可驗收實作的步驟。現有C寫入工具與Shell服務保留固定實測案例，不能直接讀取新profile。該案例的可重現安裝命令已移到[案例服務文件](examples/validated-service.md)。

## 1. 離線推導與基線

```sh
python3 scripts/plan-vlan-profile.py examples/two-access-vlans.json
python3 scripts/plan-vlan-profile.py examples/cpu-tagged-vlan.json
```

先用profile確認預期的VID／成員／Untag／PVID／Mask及CPU子介面需求，再以實際硬體基線做差異。合成baseline只用來示範，不作部署判斷。

真實盤點需要Switch Mode、VLAN Control、涉及的所有PVID／Mask、每個目標與刪除VID的Entry、管理bridge／route、既有Wi-Fi與VLAN介面。來源工具有以下界線：

| 工具 | 原有能力與移用要求 |
|---|---|
| `airport-switchctl.c` | register READ、selector編碼及固定audit；硬體操作仍要求研究復原Guard |
| `airport-vlan-table-reader.c` | VTA查詢參考，query只接受歷史18／1002／1003；查其他VID須先改白名單與基線條件，保留只有Index／READ command寫入的限制 |
| `airport-dot1q-test.c` | 具已驗證ABI與固定表項寫入流程；本身不是通用profile loader |

Query會寫VTA控制暫存器，亦可能需要回復保護。不能用Entry latch的一次register read取代指定VID查詢，也不能在缺基線時預填零。

## 2. 將新 profile 移植到硬體引擎

通用目標可以用兩類資料表示，以下是移植時的結構示意：

```c
struct vlan_change {
    uint16_t vid;
    uint32_t before_entry;
    uint32_t after_entry;
};
struct register_change {
    uint32_t selector;
    uint32_t before_value;
    uint32_t after_value;
};
```

以規劃器 `changes` 的before／after逐筆填入經審查的清單，再將固定引擎的三個VID／單一Port2路徑改為遍歷這些清單。這些結構不是現有引擎已支援的API，規劃器也不會自動產生或安裝新引擎。

改動時要同時完成：

1. 每個目標VID及其before／after Entry白名單；每個PVID／Mask selector及合法值白名單。
2. `verify/baseline` 檢查所有納入交易的欄位，而非只查原來三VID與P2/P3/P8。
3. `apply` 依新拓撲安排過渡順序，保持管理路徑，最後核對完整目標。
4. `restore` 接受本交易已知的部分完成狀態，回復真正保存的before值；不把原案例的VID18／1002／1003原值當作所有設備的快照。
5. 仍保留16-byte request、正確Acquire／Release、READ／WRITE方向、VTA busy輪詢、每次寫入Read-back及禁止整表CLEAR。

VTA查／寫共用流程可沿用，PVID／Mask的selector則按埠號公式產生。不可僅把 `TEST_VID` 改成新數字：原碼還有 `write_port2()`、固定selectors、literal、ABI字串及回復條件。

本專案的離線差異明確是不排序集合。先建完整新表項再切嚴格策略是已驗證案例的作法；涉及改Native、改管理埠或同時搬多個Access時，須按那個拓撲檢查中間狀態，不能假定任意順序皆不中斷。

## 3. 交叉建置

研究工具使用Zig0.13.0與自備相容NetBSD6 ARM SDK：headers、libc、libgcc、crt0/crti/crtbegin/crtend/crtn。以ARM freestanding EABI編譯，再使用NetBSD啟動物件靜態連結；Linux ARM ELF不能替代。

```sh
export AIRPORT_UNSAFE_RESEARCH=YES
export AIRPORT_NETBSD_SDK=/path/to/netbsd6-arm-sdk
export AIRPORT_ZIG=/path/to/zig-0.13.0
AIRPORT_TOOL_MODE=read-only sh scripts/build-airport-switchctl.sh
```

原建置模式：

| 模式 | 輸出／作用 |
|---|---|
| `read-only` | `build/airport-switchctl.netbsd`，SRAB READ參考 |
| `vlan-table-query` | `build/airport-vlan-table-reader.netbsd`，歷史查詢白名單 |
| `three-vlan-service` | `build/airport-three-vlan.netbsd`，仍是原固定案例 |

新profile引擎需新增對應的source／build mode及專用ABI識別，不能把 `three-vlan-service` 名稱直接當成通用版本。原三模式曾離線交叉編譯為32-bit ARM EABI5、static NetBSD6 ELF；原廠程式為EABI4，不推定所有SDK都相容。

## 4. 軟體 VLAN 與服務前置條件

依 `cpu_tagged_vids` 盤點哪些VID已有子介面、哪些要新增。純ASIC VLAN不用建立軟體bridge；CPU／Wi-Fi VLAN依[介面文件](interfaces-and-datapath.md)設定，並保存自己的軟體回復清單。

原服務 `prerequisites()` 寫死bridge0/bridge1、vlan0=1003及bridge0管理路由。新profile若變動CPU VLAN、bridge或管理路徑，要一起調整：

- 等待原廠ACPd完成的條件、需要保留的SSID及bridge成員。
- 設備基線／目標辨識、工具ABI及profile版本。
- 本機健康條件與新外部SSH確認，及所需端到端測試。
- 套用與還原的軟體介面交易；只還原ASIC不會自動刪除新建的vlan／bridge。

已驗證服務的可重用設計是獨立期限程序、交易token、失敗持久停用、成功Read-back才解除，以及定期判斷已知基線是否被ACPd重設。原服務仍只為原案例實作；其 `restore` 與 `stop` 不能拿來回復任意新profile。

## 5. 有期限的試運行

在碰ASIC前先啟動設備端脫離SSH的回復程序及另一個硬期限。新交易帶唯一token，舊計時器只能處理仍屬於自己的交易。套用前完整檢查before狀態，失敗或未知配置不繼續寫入。

新profile先在RAM試運行，不立即加開機掛鉤。寫入後核對全部目標，再從新外部SSH連線驗證管理仍可用，完成以下正負向測試後才結束回復交易。原研究曾遇到交叉libc的 `alarm()` 異常，不能用它取代獨立原生Shell期限。

回復程序先持久記錄停用再還原；還原失敗時，若能可靠持久停用才允許單次重啟，避免下一次開機重複失敗。Shell仍受核心排程影響，完整kernel deadlock不保證可由它復原。

## 6. 通用驗收矩陣

| 範圍 | 正向測試 | 負向／回復測試 |
|---|---|---|
| 每個Access埠 | Untagged歸入所選VID；同VID是否允許Tagged；下行去Tag | 其他已定義VID、未知VID；明確記錄Tagged admission要求 |
| 每個Trunk埠 | 所有允許VID雙向Tag及Native行為 | 未授權VID、Native與Tagged網域不混流 |
| 每對隔離VLAN | 同網域有效控制幀及真實端點流量 | 同窗口跨域注入無送達，不依賴單獨空抓包 |
| CPU／Wi-Fi | 正確子介面分流、bridge／SSID端點收發 | 不在CPU成員的VID不意外進入主機；不同bridge不互串 |
| 大小封包 | 需要的IPv4／IPv6與幀長 | 不能只用DHCP或小包Ping代替大包驗收 |
| 交易與持久化 | Read-back、完整新SSH、正常重啟 | 中途I/O失敗、未ACK、還原失敗、ACPd重設及所需冷開機 |

ASIC鏡像不等於線上封裝；Linux端點收包要同時看frame與AUXDATA。不同profile需保存自己的結果。

## 7. 離線驗證及公開檔案

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py' -v
sh -n scripts/airport-three-vlan-service.sh
sh -n scripts/build-airport-switchctl.sh
```

測試包含通用profile的VID／多埠／CPU／差異／錯誤輸入，以及原固定引擎C模擬、原服務失敗路徑與範本解析。離線計算不替代新硬體驗收。實際基線、封包與產物保持在私人或被忽略的目錄；公開範例只有合成數據。
