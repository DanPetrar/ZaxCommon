# ZaxCommon

Shared, header-only firmware modules for the **ZaxEnergy** (ZaxMonitor) and
**EnergyCalibrator** ESP32-S3 projects. Extracted so a fix to fault handling,
ring buffers, snapshots, or logging is made **once**, not duplicated per project.

## Modules (`src/`)

| Header | Purpose |
|--------|---------|
| `RingBuf.h`     | PSRAM-backed templated ring buffer |
| `ErrorLog.h`    | LittleFS-backed rolling error/event log |
| `EnergyLog.h`   | Binary append log of `MinRecord` energy counters |
| `FaultMonitor.h`| Voltage/current/PF/freq/comm fault detection |
| `Snapshot.h`    | Periodic ring-buffer persistence to LittleFS |

## Dependency contract

These headers are **not standalone**. The including sketch must provide a
`Config.h` (included first) that defines: `SecRecord`, `MinRecord`,
`DATA_VERSION`, and `ZaxConfig` (with the fault-threshold fields). That
`Config.h` **must use a macro include guard, not `#pragma once`** — it is
reached via two different paths (the sketch and this library), and
`#pragma once` dedupes by file path, which would double-define the structs.

`mqttFaultEvent()`, `faults`, and `gFaultChanged` are forward-declared here
and defined in each project's `.ino`.

## Install

Clone into your Arduino libraries directory:

```bash
git clone git@github.com:DanPetrar/ZaxCommon.git ~/Arduino/libraries/ZaxCommon
```
