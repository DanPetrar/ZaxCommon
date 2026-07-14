#pragma once
#include <Arduino.h>

// ── Persistence kill-switch + LittleFS byte budgets (shared) ──────────────────
//
// gPersistMode: 1 = FULL (LittleFS data writes enabled), 0 = OFF (skip all
// LittleFS data writes — live MQTT/Modbus only). The sketch sets this from
// cfg.persist_mode at boot and on every change; the auto-guard flips it to 0
// after repeated write failures and persists OFF to NVS. Default 1 so projects
// that don't manage it behave exactly as before.
static volatile uint8_t gPersistMode = 1;

// Per-consumer LittleFS byte budgets, derived at mount from totalBytes().
// 0 = "use the compiled default / unbounded" — the value stays 0 on partitions
// >= 1 MB, so big-flash boards (and projects that never call the computer) keep
// byte-identical prior behavior. On the 128 KB S3-Zero partition the budgets
// bind so every write always fits.
static size_t gEnergyMax  = 0;   // energy.bin rotation cap   (0 → ENERGY_MAX)
static size_t gSnapSecMax = 0;   // sec snapshot file cap     (0 → full ring)
static size_t gSnapMinMax = 0;   // min snapshot file cap     (0 → full ring)
static size_t gErrLogMax  = 0;   // errors.log rotation cap   (0 → ERR_LOG_MAX)

// Compute budgets as fractions of the LittleFS partition. Called once from
// energyLogInit() with LittleFS.totalBytes(). Partitions >= 1 MB are left
// unbounded (0) so large-flash boards are unchanged; smaller partitions get
// every consumer bounded to fit.
//
// Sizing invariant (v1.0.7): the snapshots write atomically via tmp+rename, so
// the partition must transiently hold the old file AND the new tmp of the
// largest snapshot. Hence: sum(budgets) + max(snapshot budget) + block/metadata
// slack <= total. The old 45/18/12/12 split summed to 87% and left less free
// space than one sec tmp — once every file reached its cap, the hourly sec
// snapshot failed permanently (seen on Unit_B at 81% used, 2026-07-13/14).
// New split: 40+16+10+8 = 74% steady + 16% sec tmp = 90%, ~10% slack.
static inline void persistComputeBudgets(size_t total) {
  if (total >= (1024UL * 1024UL)) {   // >= 1 MB → leave unbounded (big-flash)
    gEnergyMax = gSnapSecMax = gSnapMinMax = gErrLogMax = 0;
    return;
  }
  gEnergyMax  = total * 40 / 100;     // long-term energy log (most valuable)
  gSnapSecMax = total * 16 / 100;     // seconds ring snapshot
  gSnapMinMax = total * 10 / 100;     // minutes ring snapshot
  size_t err  = total * 8 / 100;
  gErrLogMax  = (err < 65536UL) ? err : 65536UL;   // cap at 64 KB
}
