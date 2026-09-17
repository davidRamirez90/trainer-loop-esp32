# Phase 1 — preservation and hardware evidence

Inspection date: 2026-09-16, host timezone Europe/Berlin.

## Non-destructive host evidence

The initial USB inventory was observed through macOS IORegistry without opening
the serial port, asserting DTR/RTS, entering the ROM loader, loading a stub,
halting JTAG, or writing anything. A separately authorized read-only
maintenance audit is recorded below; it used the board's already-entered ROM
loader without resetting the board, loading a flasher stub, or writing flash:

| Observation | Value |
|---|---|
| USB vendor/product | Espressif USB JTAG/serial debug unit |
| VID/PID | `0x303A:0x1001` |
| USB serial | `1C:DB:D4:7A:06:A4` (hardware identifier, not a credential) |
| Host device nodes | `/dev/cu.usbmodem2101`, `/dev/tty.usbmodem2101` |
| Evidence meaning | Native ESP32-S3 USB JTAG/serial peripheral is present |

The initial inventory alone did not identify the board revision, silicon
revision, installed firmware, flash/PSRAM size, security state, partition
table, SD card, battery, or RTC backup.

## Read-only attached-board audit

The audit ran on 2026-09-16 against `/dev/cu.usbmodem101` with `esptool`
`chip_id`, `flash_id`, `get_security_info`, and a 4 KiB partition-table read,
plus `espefuse summary`. The commands used `--before no_reset`; esptool was
also run with `--no-stub`. No full flash image was read.

| Observation | Measured value |
|---|---|
| Chip | ESP32-S3 QFN56, revision `v0.2` |
| ROM features | Wi-Fi, BLE, USB-Serial/JTAG, 40 MHz crystal |
| PSRAM | Embedded 8 MB, `AP_3v3`; eFuse `PSRAM_CAP=8M` |
| Flash JEDEC | Manufacturer `0x20`, device `0x4018`; detected 16 MB |
| Flash wiring/voltage | Quad (4 data lines), 3.3 V |
| Security state | Secure Boot disabled; Flash Encryption disabled; `SPI_BOOT_CRYPT_CNT=0`; download mode not disabled |
| Partition table source | Live flash sector `0x8000`, length `0x1000` |

The live partition table decodes as:

```text
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   24K
otadata,  data, ota,     0xf000,   8K
phy_init, data, phy,     0x11000,  4K
model,    data, spiffs,  0x12000,  952K
factory,  app,  factory, 0x100000, 4288K
ota_0,    app,  ota_0,   0x530000, 6M
storage,  data, spiffs,  0xb30000, 4884K
```

This table was captured before the later authorized bring-up work and no
post-bring-up partition-sector read is recorded. It differs from the current
project [`partitions.csv`](../../partitions.csv), which uses `app0`/`app1` at
`0x20000`/`0x420000` and a LittleFS `storage` partition at `0x820000`.
Therefore it is not evidence that an application-only update is compatible;
the live table must be re-read and the data-preservation decision made before
any firmware write.

The board MAC and optional unique ID were intentionally not recorded in this
document. The live table is the currently installed firmware's layout; it is
not evidence that the project's development partition policy has been flashed.

## V1 design audit captured from the migration spec

The selected board is the documented V1: SH8601 QSPI AMOLED and FT3168 I2C
touch. Primary source-verified pins are recorded in
[`main/board_v1.h`](../../main/board_v1.h): QSPI CS/CLK `12/11`, QSPI D0–D3
`4/5/6/7`, I2C SDA/SCL `15/14`, touch interrupt `21`, and one-bit SD
CMD/CLK/D0 `1/2/3`. USB D−/D+ GPIO19/20 remain reserved.

Display/touch reset and enables are outputs of the I2C expander at `0x20`, not
ESP32 GPIO0–2. The firmware initializes the documented P0–P2 outputs and holds
the documented P7 SD chip-select high while the shared bus is idle; P3–P6
remain inputs. GPIO0 remains BOOT and is never held during normal power-on
gestures. Physical rail sequencing and behavior remain acceptance gates.

## Open evidence gates

- The user confirms the physical board is V1 and reports no TF card is
  currently inserted. A rear-label photograph and measured TF-card/free-space
  evidence are still open.
- Visually audit expander output mapping, PMU rails/interrupt, reset order,
  RTC backup, battery/charging behavior, and PWR cutoff behavior.
- The native V1 SH8601/FT3168 path and Espressif SH8601 component `2.0.1` are
  now pinned in the project. The current Waveshare 2.0.3 board package must
  not be adopted blindly because its linked source constructs a CO5300 V2
  panel. Display colors/orientation, full-corner touch repeat, and rail
  behavior still require physical acceptance; the touch frame path itself has
  now been verified on the attached unit.
- Characterize Zwift Hub One BLE loss, pause/stop, low-target, supervision,
  and power-loss behavior before any production safety claim.

A 4 KiB partition-table sector was read to a temporary host file solely for
decoding. No full flash backup, eFuse write, JTAG halt, or credential capture
was performed by this phase.

## Subsequent authorized serial bring-up

On 2026-09-17, firmware flashing and serial monitoring were separately
authorized for live bring-up. The board enumerated as
`/dev/cu.usbmodem1101` and the 115200-baud monitor captured
`boot:0x2b (SPI_FAST_FLASH_BOOT)`, confirming that releasing BOOT and resetting
the board selects the normal application path rather than the ROM download
loader. The log also confirmed 16 MB flash, 8 MB PSRAM, successful application
entry, LittleFS first-boot formatting, journal/cache directory creation,
SH8601 panel creation, LVGL readiness, BLE initialization, and setup AP
startup at `192.168.4.1`.

The same run reported `FT3168 probe/init failed: ESP_ERR_NOT_SUPPORTED` and a
PCF85063 oscillator-stop flag. These are physical touch/RTC acceptance gates,
not boot or power failures. No full-chip erase, eFuse write, or JTAG halt was
performed; the storage partition was initialized by the firmware's explicit
format-on-mount-failure policy after the development partition subtype changed
from SPIFFS to LittleFS.

## Follow-up authorized touch bring-up

On 2026-09-17, the V1 expander sequence was corrected to the manufacturer's
ordering: configuration `0x78`, output latch `0x80`, a 20 ms reset interval,
then output latch `0x87` and a 150 ms controller-startup interval. A bounded
two-attempt repeated-start register read handles the ESP-IDF master FSM state
error observed on the first transaction.

The resulting serial run confirmed FT3168 device ID `0x03` at address `0x38`
and emitted valid press/release transitions from the LVGL input polling path.
Captured points included approximately `(186,237)`, `(65,46)`, `(334,427)`,
and `(48,419)` on the 368×448 panel. The diagnostic logs were removed after
this capture. The final non-diagnostic image was flashed with SHA verification;
no full-chip erase, eFuse write, or JTAG halt was performed.

## User-reported BLE follow-up

On 2026-09-17, the user reported that the Garmin HR monitor appears during
scanning and can be connected, but no HR values are visible on the device
screen. The trainer does not appear during scanning. This is a partial,
user-observed integration result; it does not establish HRS telemetry, UI
rendering, trainer discovery, FTMS subscriptions, or safe ERG control. Those
Phase 4/5 gates remain open and the next hardware test should isolate trainer
discovery before attempting a ride.

## Read-only maintenance follow-up — 2026-09-17

The authorized no-reset/no-stub ROM connection attempt on `/dev/cu.usbmodem2101`
failed to synchronize. No fresh flash ID, partition sector or full backup was
obtained; partition compatibility remains unresolved. No reset fallback or
device write was performed. Laptop tests passed again (CTest 2/2). See the
[inspection result and proposed backup plan](MAINTENANCE-2026-09-17.md); backup approval and
the explicit NVS/storage preservation decision remain pending.
