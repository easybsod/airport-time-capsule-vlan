# 離線配置範例

本目錄JSON是設計範例，不是目標設備的現況或部署包。ASIC P0/P1與LAN1/LAN2的個別映射必須先驗證。

| 檔案 | 示範 |
|---|---|
| [two-access-vlans.json](two-access-vlans.json) | VID200／P0 Access、VID300／P1 Access，P3共用Trunk |
| [cpu-tagged-vlan.json](cpu-tagged-vlan.json) | VID400在P1/P3/P8 Tagged，供CPU子介面使用 |
| [synthetic-baseline.json](synthetic-baseline.json) | 純合成before值，用來展示差異；不代表實際盤點 |

執行與欄位說明見[Profile文件](../docs/profiles-and-examples.md)。新的實機基線及帶私人管理資料的筆記請存入被Git忽略的私人目錄，不覆寫這些公開範例。
