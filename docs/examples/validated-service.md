# 已驗證固定案例：建置、啟動服務與回復

> 本頁只適用於既有 VID18／1002／1003、LAN3=P2 的已驗證案例。通用 profile 的設計及移植請從[通用建置與部署流程](../build-and-service.md)開始；此服務不會讀取新的 profile JSON。

本頁記錄已在 **Apple AirPort Time Capsule（2013 款，A1470），韌體版本 7.9.1** 實機證實可用的固定配置重現方式。其他型號或韌體版本尚未驗證。

**部署前須已取得裝置的 SSH 存取權限，並具備操作 `/dev/bcmsrab0`、安裝服務與修改開機配置所需的系統管理權限。本專案不提供如何開啟 SSH 的方法。** 以下所有設備端步驟均假設已能透過 SSH 登入。

下列設備端命令是操作說明，本次整理沒有在設備執行。SRAB 會影響管理網路；必須先有已驗證的原廠基線、外部登入及實體復原途徑，才進入硬體試運行。

## 交叉建置

需要 Linux 主機、Zig 0.13.0、相容 NetBSD 6 ARM 的自備 SDK。SDK 至少包含：

```text
usr/include/             NetBSD 6 ARM headers
usr/lib/crt0.o
usr/lib/crti.o
usr/lib/crtbegin.o
usr/lib/crtend.o
usr/lib/crtn.o
usr/lib/libc.a
usr/lib/libgcc.a
```

建置腳本先用 `zig cc -target arm-freestanding-eabi -fno-pie -fno-PIE`、SDK headers 編譯，再以 `zig ld.lld -static -e _start`，按 crt 啟動／收尾物件與 `-lc -lgcc` 順序連結。不使用 Linux libc；普通 `arm-linux-gnueabi-gcc` 直接產生的 Linux ELF 不能取代它。

```sh
export AIRPORT_UNSAFE_RESEARCH=YES
export AIRPORT_NETBSD_SDK=/path/to/netbsd6-arm-sdk
export AIRPORT_ZIG=/path/to/zig-0.13.0

AIRPORT_TOOL_MODE=three-vlan-service sh scripts/build-airport-switchctl.sh
AIRPORT_TOOL_MODE=read-only sh scripts/build-airport-switchctl.sh
AIRPORT_TOOL_MODE=vlan-table-query sh scripts/build-airport-switchctl.sh
```

| 模式 | 源碼 | 預設產物 |
|---|---|---|
| `three-vlan-service` | `src/airport-dot1q-test.c` | `build/airport-three-vlan.netbsd` |
| `read-only`（預設） | `src/airport-switchctl.c` | `build/airport-switchctl.netbsd` |
| `vlan-table-query` | `src/airport-vlan-table-reader.c` | `build/airport-vlan-table-reader.netbsd` |

可傳第一個參數改變輸出路徑。所有模式保留 `AIRPORT_EXPLICIT_UNSAFE_RESEARCH` 編譯條件；服務版本另定義 `AIRPORT_THREE_VLAN_SERVICE`。未設定 build opt-in、SDK 或模式無效時拒絕建置。建置只操作本機檔案。

本次已用既有 Zig0.13.0／NetBSD6 SDK 實際建置上述三種產物，`file` 均辨識為 **ELF 32-bit LSB ARM、EABI5、statically linked、NetBSD6.0**。來源原廠程式為 EABI4；此專案測試用 EABI5 工具已在來源設備運作，不代表任意工具鏈、libc 或 AirPort 韌體皆相容。本次沒有將新建置產物上傳設備重測。

來源曾遇到交叉連結 libc 的 `alarm()` 與部分 stdout 輸出異常，所以硬體引擎以 stderr 診斷，且由獨立原生 Shell 程序控制期限。本包沒有自行把 `alarm()` 補回去。

## 離線測試及其範圍

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
sh -n scripts/airport-three-vlan-service.sh
sh -n scripts/build-airport-switchctl.sh
python3 -m py_compile scripts/inspect-arm-kernel.py scripts/inspect-esconfig-switch-profile.py
```

Python 測試只需標準函式庫，C 引擎模擬器另需主機 `cc`。本案例對應的8項 unittest 全部通過，另有通用profile的獨立測試：

| 測試檔 | 檢查的行為 |
|---|---|
| `test_dot1q_engine.py`＋`dot1q_simulator.c` | 直接包含未改動 C 引擎，替換所有設備 I/O；驗證 bitmap／selector／ABI、Guard 拒絕、apply／verify／restore、半途寫入失敗回復、未知狀態拒絕及非法值白名單 |
| `test_three_vlan_service.py` | 在暫存目錄及自有程序群組模擬設備命令；首次 commit、基線被重設後重套、後續開機自動完成、未 ACK 回復、套用失敗、回復失敗持久停用及一次重啟、前置介面缺失 |
| `test_profile_parser.py` | 用合成40-byte records 驗證離線範本解析及範圍拒絕 |

測試不操作 SRAB、真實網路或真實重啟。C 模擬器能驗證程式執行路徑，不能證明真 ASIC 的 register 語意、ARM執行相容性或實體封包隔離。

## 服務與硬體引擎的分工

服務程式固定路徑：

| 路徑 | 作用 |
|---|---|
| `/mnt/Flash/airport-three-vlan/service.sh` | Shell 管理程式 |
| `/mnt/Flash/airport-three-vlan/switch-config` | 以服務模式建置的硬體引擎 |
| `.../enabled` | 允許啟動 |
| `.../trial` | 首次試運行仍需外部 ACK |
| `.../disabled` | 故障持久停用，優先於 enabled |
| `.../events.log` | 啟動／重套／故障事件；啟動時前一份改為 events.previous.log |
| `/mnt/Memory/airport-three-vlan/` | RAM中的 lock、PID、交易 token、readback、健康快照 |
| `/mnt/Flash/airport-srab-lab/armed` | 舊研究工具的互斥 marker；存在時服務拒絕開始新的交易 |

服務引擎的 `abi` 不開啟設備，會在 stderr 輸出：

```text
AIRPORT_THREE_VLAN_ABI=1 config=strict-8021q-v1 read=0xc0106d02 write=0x80106d03 size=16
```

其餘 `apply`／`restore`／`verify`／`baseline` 同時要求：

```text
AIRPORT_THREE_VLAN_GUARDED=BOUNDED_SERVICE_TRANSACTION
/mnt/Memory/airport-three-vlan/armed 存在
```

這是防止誤用的 Guard，並非權限／安全邊界。具有 root 權限仍可偽造 marker；正確流程由 Shell 先建立 token／armed 並啟動獨立回復與硬期限程序，才允許操作 ASIC。不要用手工建立 marker 代替它。

原 C 檔未定義服務巨集時，是歷史限時研究版本，要求另一組 `AIRPORT_DOT1Q_GUARDED=DETACHED_ROLLBACK` 與研究 marker。本包沒有攜入歷史 `srab-safety.sh`；此版本僅作源碼對照，建置腳本提供完整的服務模式。

## 首次試運行與開機入口

先在工作站建置，經既有已驗證的傳檔方式將 ELF 與 Shell 放到 AirPort RAM。以下以暫存路徑 `/mnt/Memory/atc-vlan-stage/` 為例。不要覆寫仍在執行的舊服務；已有安裝時先按後述停止流程核對狀態。

```sh
# 以下在已知基線的 AirPort 上執行；stage 由管理者預先傳好。
BASE=/mnt/Flash/airport-three-vlan
/bin/mkdir -p "$BASE"
/bin/cp /mnt/Memory/atc-vlan-stage/airport-three-vlan.netbsd "$BASE/switch-config"
/bin/cp /mnt/Memory/atc-vlan-stage/airport-three-vlan-service.sh "$BASE/service.sh"
/bin/chmod 700 "$BASE/switch-config" "$BASE/service.sh"
"$BASE/switch-config" abi

# 僅初次安裝：若 disabled 已存在，先查明舊故障，不在此直接移除。
test ! -f "$BASE/disabled" || exit 1
: > "$BASE/enabled"
: > "$BASE/trial"
/bin/sync
/bin/sh "$BASE/service.sh" start
```

`start` 回報 manager launched 只表示背景程序開始，不能當作套用成功。以另一個**新建立的外部 SSH 工作階段**檢查服務事件、RAM `ready` 與最新讀回，完成主網路／Guest／Access 的封包驗收後，在240秒交易期限內執行：

```sh
/bin/sh /mnt/Flash/airport-three-vlan/service.sh status
/bin/cat /mnt/Memory/airport-three-vlan/readback.log
/bin/cat /mnt/Flash/airport-three-vlan/events.log

# 新外部登入確認本次交易；commit 同時允許未來開機自動完成。
/bin/sh /mnt/Flash/airport-three-vlan/service.sh commit
```

`ack` 只接受當次試運行，保留 `trial`；`commit` 要求同樣的外部確認，並移除 `trial`。程式只能驗證 token、前置介面與 ASIC，不能自己證明這條命令真的由新 SSH 連線發出，操作者必須遵守此步驟。若期限不足，讓它回復，調查後再試，不能以舊連線提前解除保護。

首次驗收與 commit 完成後，將啟動掛鉤合併至原有 `/mnt/Flash/rc.local`。此設備 `/etc/rc.local` 是指向它的原廠連結，修改前要核對連結、備份既有內容並保留原腳本流程。下列是應加入的片段；如果已有同一標記，勿重複加入：

```sh
# AIRPORT_THREE_VLAN_SERVICE_V1
if [ -f /mnt/Flash/airport-three-vlan/enabled ] &&
   [ ! -f /mnt/Flash/airport-three-vlan/disabled ]; then
    /usr/bin/nohup /bin/sh /mnt/Flash/airport-three-vlan/service.sh start \
        </dev/null >/mnt/Memory/airport-three-vlan-boot.log 2>&1 &
fi
```

這是配合原服務的最小掛鉤示例，不聲稱與歷史設備 `rc.local` 全檔位元一致。掛鉤不能位於既有提前 `exit` 之後。不要把裸 `switch-config apply` 放入開機腳本。

## ACPd 初始化、監控與回復時序

1. 啟動前以 RAM lock 防止重複 manager，確認研究交易沒有佔用，並核對服務 ABI。manager 建立本次 token。
2. 在接觸 ASIC 前啟動兩個 `nohup /bin/sh` 獨立背景程序：240秒後回復、300秒後硬期限。token 包含時間／PID／交易名稱，舊期限只有仍匹配當次 `armed` 才生效。
3. 最多30輪、每輪間隔3秒等待 ACPd 建好 `bridge0/bridge1`、指定成員、`vlan0=1003 parent bcmeth0`、主網路有效 IPv4 及經 bridge0 的 default route。並確認硬體為已知最終配置或原廠基線。
4. 已是最終配置便核對；已知基線才 `apply`。未知配置會進入故障流程，不擅自覆蓋。
5. 套用後觀察20秒，重新核對介面與硬體；首次 trial 等新外部 ACK。正式 commit 後，後續開機通過本機檢查即可自動完成交易。
6. 每60秒開一個90秒回復／150秒硬期限的監控交易。若 ACPd 將 ASIC 重設為**已知基線**，重套配置、再觀察20秒；其他未知狀態或前置條件失敗即回復並停用。

故障流程先將 `disabled.tmp` 原子改名成 Flash `disabled` 並 sync，才停止 manager、嘗試硬體 `restore`。還原驗證成功就移除 `armed`；失敗保留 token，由另一個硬期限程序在持久停用成功後重啟一次。下次開機因 `disabled` 而不再套用，避免重啟循環。若連持久停用都寫不成功，程式不進行硬重啟。

正常監控的快照與計時資料放 RAM，啟動、重套及故障才寫 Flash。服務狀態是本機可觀察條件，沒有持續端到端 DHCP／IPv4／IPv6 健康檢查，亦未實作任意 ASIC 狀態的自動修復。完整核心 deadlock 可能使 Shell 計時器也失去排程；獨立程序不是獨立硬體 watchdog。

## 狀態、停止與重新啟用

```sh
/bin/sh /mnt/Flash/airport-three-vlan/service.sh status
/bin/sh /mnt/Flash/airport-three-vlan/service.sh stop
```

`status` 核對 manager PID 是否存在並標示交易狀態；`last-good` 明確只是最後成功 token。快取讀回不能單獨證明硬體現在仍相符。平時監控每次以真正 VTA 查詢更新 `readback.log`，查詢本身會寫控制暫存器。

`stop` 使用相同受保護回復流程，會留下持久 `disabled`，成功還原後才解除交易。檢查 `last-restore.log` 含原廠基線驗證、`armed` 已移除、manager PID 不再存活、lock 已清除及管理網路正常，再考慮重新啟用。

故障排除後重新試運行的條件是：已讀懂 disabled／事件原因，確認無活躍 manager／交易／研究 watchdog，且硬體是已知可處理狀態。保留故障紀錄後，移除 `disabled`、重新建立 `trial`，再從 `start`→新外部驗收→`commit` 跑完整流程。不要在程序仍活躍時刪除 lock／armed，或因 `enabled` 還在就直接重啟服務。

若要退役服務，先完成 `stop` 與原廠基線核對，再移除開機標記片段及 `enabled`；備份仍應保留到復原驗收結束。設備上其他 Wi-Fi／PF 調整不屬於此服務的管理範圍。

## 其他研究及驗收源碼

`airport-switchctl` 的 `abi`、`validate` 不開啟設備；硬體動作包括 `open-only`、`read-locked <page> <reg> <bits>` 及 `audit-*` 固定群組，要求 `AIRPORT_ALLOW_SRAB_READ=ARMED_WITH_RECOVERY` 與研究 marker，沒有 WRITE 呼叫路徑。`airport-vlan-table-reader` 使用 `AIRPORT_ALLOW_VLAN_TABLE_QUERY=ARMED_WITH_RECOVERY` 與同一研究 marker，CLI 為 `abi` 或 `query 18|1002|1003`。它只核對歷史基線的一部分，不能代替最終引擎的 `verify`。本包未攜入歷史研究 wrapper，沒有提供繞過 Guard 的操作命令。

兩支固定 L2 驗收源碼供檢查方法參考：

| 源碼 | 行為／限制 |
|---|---|
| `src/airport-isolation-inject.c` | AirPort BPF 從 `bcmeth0` 發兩個固定 Native／Guest 廣播；無 SRAB；參數 `send-native-guest` |
| `src/lan3-isolation-probe-linux.c` | Linux 端點 `send-five` 發五種封裝；`receive-downlink` 以 AUXDATA 接收10秒；固定綁定名為 `bridge` 的介面，移用到其他端點必須改介面綁定 |

兩者使用固定 locally administered 測試 MAC 與 EtherType `0x88B5`，不改介面／IP／路由。Linux probe 可用主機 `cc -std=gnu99 -Wall -Wextra -Werror` 建置；ARM Linux 端點需相應交叉工具鏈。AirPort BPF probe 用同 SDK 的 NetBSD 編譯／連結方式；本次只完成它的 object 編譯檢查，建置腳本不提供 probe 模式。

下行負向測試還必須由獨立 WAN1002 端點送相同來源測試 MAC 的正向控制幀；兩支 probe 本身不包含該上游發送器，不能單獨自動產生完整驗收報告。

離線反組譯另需 `pyelftools`、`capstone`：

```sh
python3 scripts/inspect-arm-kernel.py --elf /path/to/your/kernel.elf bcmsrab_ioctl
python3 scripts/inspect-esconfig-switch-profile.py --help
```

後者的 `--offset` 是自行定位的 ELF file offset，非虛擬地址；每筆操作記錄40 bytes。其 profile mask 不能直接當 `esconfig -oX` CLI 模式編號。本包不含原廠 ELF、固定映像 offset 或下載／解包程序。
