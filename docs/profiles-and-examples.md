# Profile 格式與離線範例

[規劃器](../scripts/plan-vlan-profile.py)以拓撲描述計算 Entry、PVID、Port VLAN Mask、selector 及基線差異。它只讀寫本機JSON，不使用SSH、SRAB或網路，也不產生可直接部署的硬體寫入器。

## Profile 欄位

| 欄位 | 說明 |
|---|---|
| `format` | 目前為整數1 |
| `device` | `{"model":"A1470","firmware":"7.9.1"}`，維持研究硬體／ABI範圍 |
| `entry_extra_bits` | 明示沿用本設備研究高位 `"0x00200000"`；不替此位元賦予未證實的名稱 |
| `vlans` | 每個表項含 `vid`、`members` 埠清單、`untagged` 出口清單 |
| `ports` | 明示P0/P1/P2/P3/P8的 `port`、`pvid`；可另給 `pvlan_mask` |
| `remove_vids` | 明確要清零的舊VID；不可同時存在於目標 `vlans` |

數值接受JSON整數或十六進位字串。VID範圍為1～4094，排除VID0及4095；埠清單不能重複，Untag必須是Members的子集。規劃器不把JSON布林值當整數，也拒絕未知欄位以攔截拼字錯誤。Profile與baseline中的重複JSON欄位、NaN及Infinity會被拒絕，避免後值靜默覆蓋前值。

PVID必須指向包含該埠的目標VLAN。若未給mask，就取該埠所有VLAN之成員聯集；明示mask可較寬，但不能漏必要對端或包含未納入研究的資料埠。P5不生成逐埠register修改，輸出以 `unmanaged_ports` 標示；全域Control與VLAN表項仍可能影響它，不代表整條P5路徑保持原樣。P4/P6/P7不接受為目標資料埠。

`untagged` 是輸出規則，`pvid` 是輸入預設。非對稱或多Untagged出口VID的設計會出現warning，並不被誤當成一般Access；是否真的需要這種設計由操作者決定及實測。

## 範例A：兩個 Access VLAN 共用 Trunk

完整檔案：[two-access-vlans.json](../examples/two-access-vlans.json)。

| VLAN | Members | Untagged出口 | 設計用途 |
|---:|---|---|---|
| 18 | P2/P3/P8 | P2/P3/P8 | 保留一條Native管理路徑 |
| 200 | P0/P3 | P0 | P0 Access200，P3 Tagged200 |
| 300 | P1/P3 | P1 | P1 Access300，P3 Tagged300 |
| 1003 | P3/P8 | 無 | 保留基線Guest的CPU Tagged路徑 |

P0/P1在此是ASIC編號；實際對應最上／中間哪一個LAN孔，須在目標設備確認。範例沒有將LAN3指定為新Access埠，P2仍保留Native。

```sh
python3 scripts/plan-vlan-profile.py examples/two-access-vlans.json
python3 scripts/plan-vlan-profile.py examples/two-access-vlans.json \
  --baseline examples/synthetic-baseline.json
```

主要計算結果：

| 項目 | 值 |
|---|---:|
| VID18 Entry | `0x0022190C` |
| VID200 Entry | `0x00200209` |
| VID300 Entry | `0x0020040A` |
| VID1003 Entry | `0x00200108` |
| P0 PVID／Mask | `200 / 0x009` |
| P1 PVID／Mask | `300 / 0x00A` |
| P2 PVID／Mask | `18 / 0x10C` |
| P3 PVID／Mask | `18 / 0x10F` |
| P8 PVID／Mask | `18 / 0x10C` |

這是合成設計的離線結果，沒有在AirPort套用。它保留18／1003是為示範保留既有服務；規劃器本身不把這兩個VID寫成必填常數。

## 範例B：外部與 CPU 都承載 Tagged VLAN

完整檔案：[cpu-tagged-vlan.json](../examples/cpu-tagged-vlan.json)。

保留Native成員P0/P1/P2/P3/P8，新增VID400 `members=[1,3,8]`、`untagged=[]`。結果Entry=`0x0020010A`，P1/P3可以送收Tagged400，CPU P8也可以處理它。P1/P3仍保留Native Untagged，不是只准Tagged輸入的埠。

```sh
python3 scripts/plan-vlan-profile.py examples/cpu-tagged-vlan.json
```

輸出的 `cpu_tagged_vids` 包含400及原Guest1003。400若要在CPU使用，另建NetBSD子介面，例如未占用的 `vlan1`，設定 `vlan 400 vlanif bcmeth0`；如果只需CPU端點則不必建新bridge。如果要接Wi-Fi，還須完成獨立bridge與SSID成員設計。

## 換 VID、換埠及增加 VLAN

每個VLAN以自己的members／untagged清單定義，沒有 `LAN3_TARGET_PVID` 之類的profile欄位。可以將Access改到其他確認過的埠、用不同外部埠當Trunk、同一Trunk承載多個VID、或讓多個Access埠共用同一VID。每次都要同步檢查原VLAN成員、各埠PVID及CPU需求。

若把VID200改為其他VID，除了該 `vid` 欄位，也要改引用它的PVID及軟體子介面／上游配置。規劃器會拒絕PVID仍引用不存在的舊VID。不同的完整VID配置可以完全不包含1002，且Access不必使用P2。

## 基線與差異

[synthetic-baseline.json](../examples/synthetic-baseline.json)是合成數據，刻意標記 `kind="synthetic"`。它借用已知模式值示範格式，但其中新VID的零值不是新硬體讀值。

真實盤點可使用相同結構，標記 `kind="operator_supplied"`，保存在被忽略的私人目錄。這個標記表示操作者提供，規劃器不會替它證明可信或完整：

```sh
python3 scripts/plan-vlan-profile.py examples/two-access-vlans.json \
  --baseline private-logs/device-baseline.json \
  --output private-logs/reviewed-plan.json
```

基線必須包含Switch Mode、Control0/4/5、所有受管理埠的PVID／Mask，以及每個目標／刪除VID的實際原Entry。工具不會把缺失表項填零；盤點中的非零舊VID若未在目標保留，須用 `remove_vids` 明確移除。

輸出包含：

- `target.vlans`：Members／Untag位圖、完整Entry及hex顯示。
- `target.registers`：PVID、Mask、Control的Page／Reg／寬度／selector與目標值。
- `changes`：帶基線時才提供before／after；未帶基線為null。
- `baseline_kind`：來源為synthetic、operator_supplied或not_supplied。
- `cpu_tagged_vids`：CPU Tagged成員所需要的軟體VLAN盤點清單。
- `warnings`／`limitations`：非對稱Tag、合成基線與證據界線。

`deployable=false`、`hardware_tested=false`、`changes_are_unordered=true`會始終保留。差異集合不是寫入順序，不是硬體transaction，也不是現有三VLAN服務可讀取的配置檔。

## 規劃器不替代的工作

實體映射、完整ASIC基線、未列出的舊VLAN、動態MAC學習、未知單播／多播等行為，仍須由設備盤點與封包驗收確認。原廠ACPd可能重新初始化Switch或bridge，新profile的服務前置條件與回復流程也須調整。移植方式見[建置與部署](build-and-service.md)。
