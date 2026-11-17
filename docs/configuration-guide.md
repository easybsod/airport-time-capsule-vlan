# 通用 802.1Q VLAN 配置設計

適用研究環境是 **Apple AirPort Time Capsule（2013 款，A1470），韌體 7.9.1**。部署前須已取得 SSH 存取權限及所需系統管理權限；本專案不提供如何開啟 SSH 的方法。

本流程依拓撲選擇 VID 和 ASIC 埠，不要求某個 VID 必須接某個 RJ45。原先 VLAN1002／LAN3 的成功經驗保留為[實測案例](examples/validated-three-vlan.md)，提供 ABI、過濾及回復方法的依據；任意新組合的成功不能由該案例直接推定。

## 1. 先定義廣播網域及管理路徑

對每一個 VLAN 列出：VID、應互通的實體埠、是否連接 CPU／Wi-Fi、各出口需要 Tagged 還是 Untagged。再逐埠定義無標籤輸入歸屬。上游埠須使用一致的 VID 和 Native 規則。

| 角色 | Ingress 分類 | Egress | CPU 是否需要參與 |
|---|---|---|---|
| 普通 Access 終端 | Untagged 依 PVID 歸入 VLAN V | 此 VLAN 在該埠 Untagged | 純硬體交換時不用 |
| 多 VID Trunk | 依收到的802.1Q Tag；Native Untagged 另看 PVID | 各 VID 多為 Tagged，可保留一個 Native Untagged | 依主機／Wi-Fi需求 |
| CPU VLAN 主機／軟體橋 | CPU parent 收到 Tagged，交給 `vlanX` | `vlanX` 經 parent Tagged 送回 ASIC | 必須加入對應 CPU 埠 |

管理連線本身也占用一個廣播網域。記下管理地址位於哪個軟體介面、經哪個實體埠到達；檢查新 membership、PVID 與 strict policy 是否仍保留這條路徑。把管理地址或實際端點資料放在私人檔案中，公開 profile 不需要它們。

## 2. 用 ASIC 編號描述埠

此研究使用外部 P0、P1、P2、P3 與 CPU P8。LAN3=P2 已拔插確認；P3=WAN、P8=CPU0 有多項佐證。P0/P1 分別是 LAN1／LAN2 中哪個孔仍要實測。完整機背位置見[接孔圖](ports-and-layout.md)。

選 Access 或 Trunk 時使用已確認的 ASIC 編號。Trunk 可位於不同外部埠，P0/P1/P2/P3 不是固定的 VLAN 角色。P5 為另一條內部通道，本規劃器不生成其逐埠register修改；全域過濾與VLAN表項仍可能影響它，不能宣稱整條路徑保持原樣。P4/P6/P7 未納入這台設備的已研究資料埠，不能因9-bit位圖有這些位置就假定它們可用。

NetBSD 的 `bcmeth0` 是 CPU parent，不是某個 RJ45。不要把 `ifconfig` 的介面名稱、外殼 LAN 序號、PHY 顯示索引與 ASIC port number 混用。

## 3. 為每個 VLAN 定義兩個集合

令 `M(V)` 是 VLAN V 的成員埠集合，`U(V)` 是其中無標籤出口集合。必須有 `U(V) ⊆ M(V)`。

```text
members_bitmap = OR(1 << p), p ∈ M(V)
untag_bitmap   = OR(1 << p), p ∈ U(V)
Entry(V) = preserved_extra_bits | members_bitmap | (untag_bitmap << 9)
```

各埠序號只出現一次時，OR 與加總相同。位圖不是 VID 清單。此設備沿用的高位是 `0x00200000`；bit21 的完整語意尚未確證，不能泛稱 VALID 或照搬到其他晶片。

以**設計範例** VID200、P0 Access、P3 Trunk 為例：

```text
M(200) = {0,3} → 0x009
U(200) = {0}   → 0x001
Entry(200) = 0x00200000 | 0x009 | (0x001 << 9)
           = 0x00200209
```

另一個 VID300 可用 P1 Access、P3 Trunk：`M={1,3}`、`U={1}`，Entry=`0x0020040A`。兩者共用 Trunk，彼此仍由不同 VLAN membership 分隔。這些是離線推導值，沒有標成新硬體驗收結果。

## 4. 定義每個埠的 PVID 及 Port VLAN Mask

PVID 決定無標籤輸入的 VLAN。普通 Access 埠設定 `PVID(p)=V`，並將 p 放入該 VLAN 的 `U(V)`；同時從不應再參與的原 VLAN 的 member／untag位圖中移除 p。

Trunk 的 PVID 決定它收到 Untagged 的分類，並不限制它只能參與那一個 VID。若保留 Native 網路，該埠通常在 Native VLAN 的 Untag 集合內，在其他 VLAN 的成員中但不在 Untag 集合內。

Port VLAN Mask 描述該埠可轉送的埠集合。通用規劃器以以下方式推導足以承載所有指定 VLAN 的 mask：

```text
Mask(p) = OR members_bitmap(V)，對所有包含 p 的 V
```

共用 Trunk／CPU 的 Mask 是多個 VLAN 對端的聯集，不是把它們合併成一個廣播網域；逐 VID 的 membership／ingress policy 仍負責隔離。也可在profile明示一個包含這個聯集的較寬mask，用於保留已知基線習慣；規劃器拒絕漏掉必要對端的mask。

一般 Access／Native 設計中，每埠只有一個 Untagged egress VID，且與 PVID 一致。硬體表格允許描述其他非對稱組合；規劃器會提出警示，讓操作者確認是否刻意設計。PVID並不等於「只接受Untagged」，要拒絕全部Tagged ingress仍需另研究frame admission。

通用 register 位址為：

```text
PVID(p)  = Page34 / Reg(0x10 + 2*p) / 16-bit
Mask(p)  = Page31 / Reg(2*p)        / 16-bit
selector = (page << 24) | (reg << 16) | width_code
```

16-bit的width_code為1；完整ABI見[SRAB文件](srab-ioctl-direction.md)。

## 5. 決定 CPU 與軟體介面

若某 VLAN 只在兩個或多個實體埠之間交換，可以不讓 P8 參與。這時 ASIC 直接轉送，不需要為該 VID 建 `vlanX`／`bridgeX`，CPU抓包也可能看不到它。

若需要 AirPort 主機協定棧、Wi-Fi 或軟體橋參與，將P8加入 `M(V)`，通常不加入 `U(V)`，讓同一CPU parent以Tag區分多個VLAN。再建立指定VID的NetBSD子介面，接到需要的軟體橋；細節見[介面對照](interfaces-and-datapath.md)。

例如 VID400、外部 P1/P3 與CPU Tagged：`M={1,3,8}`、`U={}`，Entry=`0x0020010A`。這不會自動建立Wi-Fi SSID、分配IP或新增DHCP服務；SSID由原廠管理程序建立，bridge成員也須另外盤點與維護。

## 6. 保留所有需要的既有 VLAN

此研究的原廠模式使用內部 Native VID18，以及 CPU `vlan0` 的 Guest VID1003。這些是這個基線的實際配置，不是802.1Q強制值。若保留原有主網路／Guest，就將它們連同新VID一起納入完整目標profile；若要更換它們，還須同步改軟體介面、管理及原廠初始化行為。

原廠DT模式下1003的ASIC Entry曾為零，仍能使用Guest。轉到一般標準嚴格模式後，需為要保留的Guest建立明確成員，不能只依照原廠零值省略它。

Control4=`0x40`、Control5=`0x18` 是本研究已驗證的DT關閉、來源成員違規Drop及Table Miss Drop策略。它們是**全域**設定，會影響所有VLAN；在切換前先安排完整的目標表項，並審查未處理的舊Entry。最終引擎不需把TPID暫存器改成0x8100。

## 7. 以基線產生差異及回復資料

規劃器可不帶基線產生目標值，但不會填入假定的舊值。對每個目標或明確刪除的VID，必須另外提供實際查到的原Entry；「預期沒有配置」不等於讀到零。還需保存所有涉及的PVID、Mask、Control值。

baseline中有非零舊VID時，要在目標中保留或在 `remove_vids` 明確列出。未盤點到的Entry仍可能存在，離線JSON不能證明已完整掃描硬體。不要直接將合成範例基線改名，當作實測snapshot。

輸出每個變動的before／after，作為白名單與回復值的輸入。它不是原子的交換器交易，亦不是可照順序直接執行的寫入腳本。更新多埠、改Native／管理路徑或移除舊VLAN時，須另設計過渡狀態、限時回復與外部可達性確認。

## 8. 按目標角色驗收

對每個Access埠測Untagged輸入、同VID Tagged、其他已定義VID、未定義VID；對每個Trunk測各允許VID的雙向封裝與Native行為。對每對應隔離的網域做正向控制與負向測試，不能用一個安靜的抓包窗口當作隔離成功。

CPU／Wi-Fi參與時另測子介面分流、bridge成員與真實無線端點。最後測失敗回復、原廠管理程序重設後的處理、正常重啟及需要的冷開機。每個新profile保存自己的測試結果，不把歷史案例的成功複製到新VID／埠組合。
