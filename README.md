# AirPort Time Capsule 802.1Q VLAN 通用配置指引

本專案說明如何為 **Apple AirPort Time Capsule（2013 款，A1470／model119，韌體 7.9.1）** 設計不同 VLAN ID、Access／Trunk 埠組合，以及 CPU／Wi-Fi 參與的二層網路。研究平台為 NetBSD 6.0、BCM5301x Rev 1；此環境已有實機可用案例，其他型號或韌體尚未驗證。

**部署前須已取得裝置的 SSH 存取權限，並具備執行配置的系統管理權限。本專案不提供如何開啟 SSH 的方法。**

配置由兩部分組成：以 NetBSD `vlan`／`bridge` 處理 CPU 或 Wi-Fi 流量，以交換器 ASIC 的 VLAN membership、PVID、Egress Untag 及 Ingress 過濾控制實體埠。VLAN ID 與接入埠都由拓撲決定；VLAN1002／LAN3 是[已驗證案例](docs/examples/validated-three-vlan.md)，不是通用方法的必要條件。

## 從你的拓撲開始

1. 依[機背佈局](docs/ports-and-layout.md)識別插孔，核對實體孔與 ASIC 埠號。
2. 為每個 VLAN 列出 VID、成員埠、Untagged 出口；再指定各埠 PVID，決定是否需要 CPU／軟體 VLAN。
3. 參考[通用設計流程](docs/configuration-guide.md)與 [JSON profile 範例](docs/profiles-and-examples.md)，計算表項、Port Mask 及軟體介面需求。
4. 先保存目標設備實際基線，再審查配置差異、移植硬體引擎與回復流程，按[部署及驗收步驟](docs/build-and-service.md)試運行。

通用規劃器可以離線處理多個 VID、多個 Access 埠、任意已確認的外部 Trunk 埠及 CPU Tagged VLAN。它不連線設備，輸出是設計資料；附帶的歷史硬體引擎與開機服務仍是固定案例實作，需按 profile 移植後才能用於新拓撲。

```sh
# 例：P0 Access VLAN200、P1 Access VLAN300、P3 共用 Trunk
python3 scripts/plan-vlan-profile.py examples/two-access-vlans.json

# 用合成基線展示 before／after；這不是設備讀值
python3 scripts/plan-vlan-profile.py examples/two-access-vlans.json \
  --baseline examples/synthetic-baseline.json

# 例：VID400 Tagged 到 P1、P3 與 CPU P8
python3 scripts/plan-vlan-profile.py examples/cpu-tagged-vlan.json
```

## 文件入口

| 文件 | 內容 |
|---|---|
| [通用配置設計](docs/configuration-guide.md) | 拓撲、VID、Access／Trunk、PVID、位圖、CPU 成員與回復設計 |
| [Profile 格式與範例](docs/profiles-and-examples.md) | 離線規劃器、多 Access／CPU Trunk 範例、差異及限制 |
| [介面與 Linux 對照](docs/interfaces-and-datapath.md) | NetBSD 子介面、軟體橋、Wi-Fi、Tag 路徑、MTU |
| [機背佈局](docs/ports-and-layout.md) | 實體孔、ASIC 埠號及映射證據 |
| [SRAB ABI 與暫存器](docs/srab-ioctl-direction.md) | ioctl、selector、通用位址／Entry 公式與已知控制位元 |
| [建置、部署及驗收](docs/build-and-service.md) | 從 profile 移植 C 引擎／服務、試運行與驗收矩陣 |
| [已驗證案例](docs/examples/validated-three-vlan.md) | 原三 VLAN 的數值、封包證據及固定服務入口 |
| [驗證範圍與限制](docs/validation.md) | 已驗證案例的行為、觀測方法與待驗證項目 |

## 機背位置與可配置角色

正常直立、面對機背，由上至下如下。LAN1～3 為本文按位置使用的名稱。

```text
         機頂
    ┌───────────────────┐
    │ LAN1  RJ45        │  P0/P1 之一：需確認個別映射
    │ LAN2  RJ45        │  P0/P1 之一：需確認個別映射
    │ LAN3  RJ45        │  P2：USB 正上方，映射已確認
    │ USB   Type-A      │
    │ WAN   RJ45        │  P3：有原廠範本／資料路徑佐證
    │ AC電源   ○ Reset  │
    └───────────────────┘
         機底
```

四個 RJ45 均為 Gigabit Ethernet；位置依 [Apple 官方設定指南第6～7頁](https://cdsassets.apple.com/live/6GJYWVAV/user/ma1645_airport_time_capsule_80211ac_setup.pdf#page=6) 核對。LAN3 的上下位置另經研究設備使用者確認。ASIC P8 是 CPU0 使用中的內部通道，沒有對外插孔。

Access／Trunk 是配置後的角色，可以指定給已確認映射的外部埠；外殼標示 WAN 不代表二層 Trunk 必須使用它，也不會自動增加 NAT 或路由。P0/P1 的個別實體對應尚未獨立驗證。

## 設計時使用的參數

| 參數 | 作用 |
|---|---|
| 每個 VLAN 的 `vid` | 1～4094 的識別碼；避免與保留的管理／Guest 服務衝突 |
| `members` | 該 VID 允許參與的 ASIC 埠集合 |
| `untagged` | 成員中要無標籤送出的出口集合 |
| 每個埠的 `pvid` | 無標籤輸入的預設 VID |
| Port VLAN Mask | 埠間可轉送集合；通常取該埠所有 VLAN 成員的聯集 |
| CPU membership | CPU 是否需要處理該 VLAN；CPU Tagged 成員要有對應軟體子介面 |
| Control4／Control5 | 此研究的標準嚴格模式使用 `0x40`／`0x18`；影響整台交換器 |

單純改 PVID 不會完成隔離；單純建立 `vlanX` 也不會設定實體孔。是否讓同 VID Tagged ingress 通過，以及是否要求只准 Untagged，須分別定義與驗收。

## 源碼與工具

| 路徑 | 用途 |
|---|---|
| [plan-vlan-profile.py](scripts/plan-vlan-profile.py) | 通用離線 profile 驗證、Entry／selector／Port Mask 計算及基線差異 |
| [examples](examples/) | 不綁定原案例 VID／埠的合成設計與基線 |
| [airport-switchctl.c](src/airport-switchctl.c) | SRAB register／MIB READ 參考 |
| [airport-vlan-table-reader.c](src/airport-vlan-table-reader.c) | VTA 查詢參考，仍保留歷史 VID 白名單及部分基線條件 |
| [airport-dot1q-test.c](src/airport-dot1q-test.c) | 已驗證固定案例的 apply／verify／restore；新 profile 的移植參考 |
| [airport-three-vlan-service.sh](scripts/airport-three-vlan-service.sh) | 已驗證固定案例的交易期限、開機與監控服務 |
| [build-airport-switchctl.sh](scripts/build-airport-switchctl.sh) | 原有三種 NetBSD ARM 工具建置 |
| [tests](tests/) | 通用 profile 計算與固定案例 C／Shell 行為測試 |

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py' -v
```

本指引的通用性是配置方法與離線推導可套用到不同拓撲；個別新組合仍須實機驗證。

## 授權

本專案源碼與自行撰寫的文件採用 [MIT License](LICENSE)。不包含原廠韌體、SDK、私人封包或已編譯設備 ELF；外部參考資料仍依其原有條款使用。
