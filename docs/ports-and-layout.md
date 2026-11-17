# 機背連接埠佈局與接線

適用於已驗證的 **Apple AirPort Time Capsule（2013 款，A1470），韌體 7.9.1**。部署仍以前置取得 SSH 存取權限為條件；本專案不提供如何開啟 SSH 的方法。

## 機背視角

將機器正常直立，**面對連接線所在的機背**觀察，由上至下是三個 LAN、USB、WAN、AC 電源；Reset 小孔在電源孔旁。下圖僅表示排列，不按比例繪製。

```text
                      機頂
          ┌─────────────────────────┐
          │                         │
          │     ┌───────────┐       │
          │     │ LAN1 RJ45 │       │  LAN 最上方
          │     ├───────────┤       │
          │     │ LAN2 RJ45 │       │  LAN 中間
          │     ├───────────┤       │
          │     │ LAN3 RJ45 │       │  LAN 最下方 → ASIC P2
          │     └───────────┘       │
          │     ┌───────────┐       │
          │     │ USB Type-A│       │
          │     └───────────┘       │
          │     ┌───────────┐       │
          │     │ WAN  RJ45 │       │  USB 下方 → ASIC P3
          │     └───────────┘       │
          │     AC 電源   ○ Reset   │
          └─────────────────────────┘
                      機底
```

外觀排列依 [Apple 官方 AirPort Time Capsule 設定指南第6～7頁](https://cdsassets.apple.com/live/6GJYWVAV/user/ma1645_airport_time_capsule_80211ac_setup.pdf#page=6) 核對。LAN 使用雙向箭頭圖示，WAN 使用圓環狀圖示；四個 RJ45 均為 Gigabit Ethernet，另有一個 USB 2.0 Type-A 與 AC 電源孔。介面規格亦見 [Apple 官方技術規格](https://support.apple.com/en-us/112420)。

本文為方便接線，將三個 LAN 孔由上至下稱為 **LAN1、LAN2、LAN3**；Apple 官方示意圖將三者標成 LAN 群組，沒有提供 ASIC 編號。**研究設備使用者已確認：本研究所稱的 LAN3 就是最下方、USB 正上方的 LAN 孔。** 這項外觀確認與原研究 LAN3 拔插時只有 ASIC Link bit2 變化的證據相互銜接。

## 實體孔、ASIC 編號與可配置角色

| 面對機背的位置 | 本文名稱 | ASIC 埠 | 通用設計時的角色 |
|---|---|---|---|
| 三個 LAN 中最上方 | LAN1 | P0/P1 之一，個別對應未驗證 | 確認映射後，可依profile設為Access、Trunk或保留Native |
| 三個 LAN 中間 | LAN2 | P0/P1 之一，個別對應未驗證 | 確認映射後，可依profile設為Access、Trunk或保留Native |
| 三個 LAN 中最下方，USB 正上方 | **LAN3** | **P2，已實體拔插確認** | 角色與PVID由profile決定，沒有綁定特定VID |
| USB 下方的獨立 RJ45 | WAN | P3，有原廠範本及流量等佐證；未另拔 WAN 驗證 | 可作Trunk或其他二層角色，須保留所需管理路徑 |
| 無對外插孔 | CPU0 資料通道 | P8，有讀回及資料路徑佐證 | 需要CPU／Wi-Fi的VLAN才加入，通常Tagged接軟體子介面 |

**實體 LAN3 對應的是 ASIC P2。** `LAN3` 的3是本文的接孔名稱，不能直接帶入 `PVID(port)` 等 register 位址公式當成 `port=3`。P3 用於 WAN；P5/P8 是內部通道，不是額外 RJ45。

## 依目標拓撲接線

先依profile決定哪個外部埠是Access、哪個承載Trunk，再讓上游或終端匹配它的VID、Tagged／Untagged及Native規則。Trunk不必綁定WAN孔，Access也不必綁定LAN3。WAN是外殼標示的接孔名稱，二層配置不會因此自動新增路由或NAT。

普通Untagged終端接到 `PVID=V` 且該VLAN的Untag位圖包含此埠的Access孔；會自行加Tag的交換器／主機接到允許相同VID的Trunk孔。PVID／Untag不是拒絕所有Tagged ingress的機制，同VID是否通行仍應列入驗收。

CPU／Wi-Fi需參與的VLAN另設P8 membership與軟體子介面。純外部埠的VLAN可以在ASIC直接交換，不必建立bridge。原來VID1002接LAN3的接線與數值保留於[已驗證案例](examples/validated-three-vlan.md)。

更動P0/P1前仍須確認個別實體對應，不能只按外觀順序推算。完整設計見[通用流程](configuration-guide.md)，軟體分工見[介面及資料路徑](interfaces-and-datapath.md)，數值與位址見[SRAB說明](srab-ioctl-direction.md)。
