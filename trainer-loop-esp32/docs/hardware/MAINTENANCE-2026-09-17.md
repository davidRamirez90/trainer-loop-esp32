# Read-only maintenance follow-up — 2026-09-17

Baseline: `c124000` (`feat: harden reboot-safe ride recovery`). The project
subtree was clean; unrelated untracked siblings in the parent repository were
left untouched. This record authorizes no future device operation.

## Observed result

- macOS enumerated the Espressif native USB JTAG/serial interface, VID/PID
  `303a:1001`, at `/dev/cu.usbmodem2101`. Its USB identifier matched the earlier
  inventory. Enumeration does not identify the installed application.
- Installed esptool is 4.12.0. Its source was inspected for reset behavior.
  One bounded ROM synchronization attempt used the ESP32-S3 ROM API with
  `mode='no_reset', attempts=1`. The serial object was configured with DTR and
  RTS false **before opening**, at 115200 baud, and closed afterward. No stub
  was loaded, no reset strategy was executed, and no reset fallback was used.
- Synchronization failed with `FatalError`. Raw serial/error text was suppressed
  to avoid exposing possible runtime secrets. This does not establish whether
  the board was running the application or why synchronization failed.
- Consequently **no fresh flash ID, partition sector, or full backup was read**.
  The old partition mismatch remains unresolved. Do not treat the historical
  table as current, or the later bring-up logs as a replacement for a fresh read.
- No flashing, erase, provisioning, security/eFuse changes, deliberate power
  cuts, corruption tests, or trainer commands were performed.

## Laptop assumptions checked

The generated partition binary decodes to the current source table:
`nvs` at `0x9000` (24 KiB), `otadata` at `0xf000` (8 KiB), `app0` at
`0x20000` (4 MiB), `app1` at `0x420000` (4 MiB), and LittleFS `storage`
at `0x820000` (7104 KiB). These are **build artifacts, not live flash evidence**.

`build/flash_args` includes the bootloader, application, partition table **and
initial OTA metadata at `0xf000`**. Therefore ordinary `idf.py flash` is not
an application-only operation. Matching tables alone would still not establish
which OTA slot is active or whether overwriting a slot is appropriate.

The current mount implementation enables initial formatting only after checking
that the entire storage partition is erased. It preserves a nonempty partition
when mounting fails. The earlier first-boot formatting observation predates this
guard and does not physically validate the guard itself.

Observed artifact SHA-256 values (reverify immediately before any future write):

| Artifact | SHA-256 |
|---|---|
| Partition table | `5d69bc04e955604cd52e8a7f65a0705f295dd6453618e87344965757db37ae49` |
| Application | `cdc33908e196bfa4144d7ce6a297300c9b8a3bb39103e180d87f76bf5b68b3d1` |
| Bootloader | `e7f423e98884744dbe6a1a116625aecfa550e8aa516494b3a3d165b29ba33b3a` |

Laptop validation rerun: `env -u IDF_PATH make test` passed, including Debug
CMake build and CTest 2/2 (core and independent Garmin FIT fixture validation).
No new firmware build or real ride-file validation was performed in this follow-up.

## Proposed backup plan — awaiting approval, not executed

1. Arrange an explicitly authorized way to make the ROM loader available.
   The present no-reset attempt failed; no automatic reset or manual boot/reset
   gesture is assumed authorized. Reconfirm the port after any approved change.
2. Re-read flash ID and save the 4 KiB table sector at the previously recorded
   `0x8000` location using no-reset/no-stub operation. Validate table structure,
   checksum, bounds and alignment; do not infer a usable layout from malformed
   data. Reconfirm read-only security information before selecting backup and
   restore procedures. Do not read key material or burn eFuses.
3. Obtain approval for a full raw backup into a user-approved private local
   directory outside Git and cloud-synced folders. Use directory mode 0700,
   file mode 0600, and encrypted host storage confirmed by the user. Treat the
   image as containing credentials and personal data; never print its contents,
   attach it to chat, upload it, or commit it. Approve destination/retention first.
4. Read the complete **freshly detected** flash capacity with ROM reads, no stub
   or reset. If ROM reads are unsupported or fail, stop; do not silently switch
   to a stub. Preserve bootloader, table, OTA metadata, all app/data partitions,
   gaps and trailing bytes. Historical 16 MiB is not a substitute for measurement.
5. Obtain two complete reads while flash is quiescent; require exact expected
   length and matching SHA-256. Keep an access-restricted manifest containing
   capture time, tool/version/options, size, hashes and partition decode. A
   mismatch or partial read invalidates backup acceptance. Backups do not include
   eFuses and are not proof of a tested restore; no restore test is authorized.
6. Decide explicitly whether NVS/settings and ride/cache/storage data must
   survive. Until decided, preserve everything. If preservation is required,
   stop and design a compatible image/migration from the freshly observed
   layout and storage format. A full backup alone does not make migration safe.
7. If discard is explicitly allowed, separately approve a concrete replacement
   and storage-initialization procedure derived from the fresh layout, including
   OTA selection/metadata handling. Never improvise erase offsets. Before any
   write, create a final Git checkpoint and verify the exact build artifacts.

After a separately authorized and verified update, limit the first hardware
check to boot/mount/no panic and BLE scanning with HR display observation.
Trainer discovery, FTMS capabilities, telemetry, acknowledgements and safe
pause/stop must be validated before any resistance workout. The older-image
HR/trainer reports have no current-image reproduction or captured packets;
no root cause or fix is claimed.

## Subsequent replacement authorization and pre-write checkpoint

The user explicitly decided that no settings, rides or other data must survive,
refused a backup that might contain credentials, and then authorized the actions
needed to run the latest firmware. The backup proposal above is superseded:
**no full image or credential-bearing data backup will be made**. Web setup must
remain available for private reconfiguration.

The newly authorized USB reset entered the ROM loader successfully. Fresh
no-stub measurements confirmed ESP32-S3 v0.2, flash JEDEC `20:4018`, 16 MiB,
Secure Boot disabled and Flash Encryption disabled (crypt count zero).
A no-reset/no-stub 4096-byte read at `0x8000`, saved locally as
`/tmp/trainer-loop-maintenance-20260917/partition-before.bin`, decoded and
verified successfully. It matches the current project table exactly. The
historical factory/SPIFFS layout is therefore stale, not the current layout.
Only partition metadata was saved; no NVS/application/storage backup was read.

Selected replacement procedure: full-chip erase, then write the generated
`build/flash_args` manifest (bootloader, partition table, initial OTA metadata,
and application), with esptool hash verification. This clears old data and
both OTA slots before initialization, without ad hoc region erasure or stale
OTA selection. No eFuse/security configuration changes are included.

Before device writes, `idf.py build` passed at the existing source baseline:
app `0x21c5d0`, bootloader `0x5160`. Laptop core and CTest 2/2 passed in the
preceding maintenance phase. A Git checkpoint of this record and existing
evidence edits is required immediately before executing replacement.

Independent reviews confirmed the clean NVS path starts protected web setup;
BLE starts only after private setup completion. Boot/mount/setup readiness can
be checked without credentials; BLE/HR validation remains pending that user step.
