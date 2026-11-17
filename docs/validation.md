# 驗證範圍與技術限制

實機環境為 **Apple AirPort Time Capsule（2013 款，A1470），韌體 7.9.1**。部署須事先取得SSH存取權限；本專案不提供如何開啟 SSH 的方法。其他型號或韌體版本尚未驗證。

## 已驗證案例

[固定三VLAN案例](examples/validated-three-vlan.md)有以下觀測結果；通用profile及其合成範例不因這些結果而自動視為實測通過。

| 項目 | 觀測結果與判讀 |
|---|---|
| ASIC目標值 | 寫入後及實際重新啟動後，Control4/5、三個VLAN Entry與P2 PVID／Mask讀回相符 |
| LAN3租約 | 同一transaction及client的Discover→Offer→Request→ACK完整配對，IPv4／UDP checksum有效 |
| LAN3上行隔離 | Untagged與同VID1002 Tagged測試通行，VID18／1003／4094未跨域；CPU0／Guest子介面同窗口未收到跨域測試幀 |
| LAN3下行隔離及去Tag | Native／Guest負向幀未到達；同窗口WAN1002正向幀到達，接收frame與Linux AUXDATA均無VLAN |
| 主網路及Guest | 管理登入、主網路連通及Guest DHCP成功，既有bridge成員保留 |
| IPv4／IPv6 | LAN3取得租約後，IPv4閘道、IPv4公網與IPv6公網測試均成功 |
| 開機恢復 | 實際重新啟動時觀察到新boot time、SSH失去後恢復及服務自動完成交易；不需再次人工ACK |

負向測試均搭配正向控制幀，避免將無流量的接收窗口誤當成隔離證明。CPU或上游VLAN邏輯介面的PCAP可能已去Tag，不能單靠它判定實體WAN線路的封裝。

## 實體埠映射

LAN3位於面對機背三個LAN孔最下方、USB正上方，位置由研究設備使用者確認。原研究拔除／插回LAN3時，ASIC Link Status只變化bit2，確證LAN3=P2。

P3=WAN及P8=CPU0有原廠範本、流量計數與資料路徑等佐證；P3尚未另以實際拔WAN線獨立驗證。P0/P1分別對應LAN1／LAN2的順序未驗證。P5的其他內部用途未完整解析。

## Wi-Fi、封裝與MTU

主5GHz及Guest2.4GHz有與WAN配對的實際終端流量；主2.4GHz及Guest5GHz尚缺完整端點驗收。四個SSID介面Running及bridge成員保留，不等於所有頻段已驗收。

原廠DT模式的可控校準顯示：Untagged原幀在ASIC鏡像可帶額外 `0x9100/18`，原本Tagged1003也可在鏡像多出該外層。鏡像Service Tag不能直接代表實體線路的封裝。去Tag結論應以端點收到的frame及VLAN metadata確認。

既有軟體介面的MTU讀值為1500，但不等於已量出所有VLAN路徑的最大無損幀長。原廠PF限制Guest對AirPort本機位址的存取，從AirPort自身Guest位址發起Ping也不能代表無線客戶端轉送結果。

## 未涵蓋的行為

實體斷電冷開機、上述兩個Wi-Fi頻段、任意新VID／埠組合、其他韌體、多層Tag、VID0與嚴格拒絕所有Tagged ingress等條件尚未完整驗證。新profile應依[通用驗收矩陣](build-and-service.md)取得自己的結果。

離線規劃器與測試驗證參數推導及程式行為，不會證明實際ASIC狀態。固定案例服務的獨立Shell期限仍依賴核心排程，不能保證從完整kernel deadlock復原。
