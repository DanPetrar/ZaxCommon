#pragma once
#include <LittleFS.h>
#include "Config.h"
#include "PersistBudget.h"

// Binary append log of MinRecord structs.
// File: /energy.bin  (32 bytes per record, no header)
// Capped at ENERGY_MAX (1.2 MB) → 1.2e6 / 32 ≈ 37,500 records ≈ 26 days at
// 1 rec/min. (The LittleFS partition itself is larger; ENERGY_MAX is the cap.)

static const char  ENERGY_FILE[]    = "/energy.bin";
static const char  PREV_BOX_FILE[]  = "/prev_box.bin";  // 24 bytes: float[3] kwh + float[3] kvarh
static const size_t ENERGY_MAX      = 1200000UL;  // ~1.2 MB cap (~26 days)

// Separate flag from the global `lfsOk` used by ErrorLog/Snapshot. Both are set
// together via `lfsOk = energyLogInit();` in the sketch — keep that call intact,
// or the two views of "is LittleFS mounted" will diverge silently.
static bool _lfsOk = false;

static bool energyLogInit() {
  if (!LittleFS.begin(true)) {   // true = format on fail
    Serial.println("[LFS] Mount/format failed");
    _lfsOk = false;
    return false;
  }
  _lfsOk = true;
  size_t used  = LittleFS.usedBytes();
  size_t total = LittleFS.totalBytes();
  // Derive per-consumer LittleFS budgets from the real partition size. Small
  // partitions (S3-Zero 128 KB) get every consumer bounded so writes always
  // fit; partitions >= 1 MB are left unbounded (prior behavior).
  persistComputeBudgets(total);
  uint32_t recs = 0;
  if (LittleFS.exists(ENERGY_FILE)) {
    File f = LittleFS.open(ENERGY_FILE, "r");
    if (f) { recs = f.size() / sizeof(MinRecord); f.close(); }
  }
  Serial.printf("[LFS] Mounted — %u/%u bytes, %u energy records stored\n",
                (unsigned)used, (unsigned)total, (unsigned)recs);
  return true;
}

// Rotate energy.bin if it has reached its cap: keep the newest half.
// Cap is the runtime budget (gEnergyMax) when set, else the compiled ENERGY_MAX.
// Returns true if the file is now under cap (or didn't need rotating).
static bool _energyRotate() {
  size_t cap = gEnergyMax ? gEnergyMax : ENERGY_MAX;
  if (!LittleFS.exists(ENERGY_FILE)) return true;
  File f = LittleFS.open(ENERGY_FILE, "r");
  if (!f) return true;
  if (f.size() < cap) { f.close(); return true; }
  // Rotation buffer is ps_malloc(keep); if PSRAM is absent/exhausted the alloc
  // fails and rotation is skipped — the file then grows past cap. Acceptable on
  // boards with PSRAM (all current targets); revisit if porting to a no-PSRAM board.
  size_t sz    = f.size();
  size_t keep  = sz / 2;
  size_t skip  = (sz - keep) / sizeof(MinRecord) * sizeof(MinRecord);  // align
  keep         = sz - skip;
  uint8_t* tmp = (uint8_t*)ps_malloc(keep);
  if (!tmp) { f.close(); return false; }
  f.seek(skip);
  f.read(tmp, keep);
  f.close();
  LittleFS.remove(ENERGY_FILE);
  File nf = LittleFS.open(ENERGY_FILE, "w");
  bool ok = false;
  if (nf) { ok = (nf.write(tmp, keep) == keep); nf.close(); }
  free(tmp);
  Serial.printf("[LFS] Rotated energy.bin: kept %u bytes\n", (unsigned)keep);
  return ok;
}

// Append one MinRecord. Skipped (treated as success) when persistence is OFF or
// LittleFS is down. Returns true only on a confirmed full-size write — the
// caller feeds this to the persist guard. On a short write (partition full) it
// force-rotates once and retries before giving up.
static bool energyLogAppend(const MinRecord& rec) {
  if (!_lfsOk || gPersistMode == 0) return true;

  _energyRotate();

  for (int attempt = 0; attempt < 2; attempt++) {
    File f = LittleFS.open(ENERGY_FILE, "a");
    if (f) {
      size_t n = f.write((const uint8_t*)&rec, sizeof(MinRecord));
      f.close();
      if (n == sizeof(MinRecord)) return true;
    }
    // Short write or open failed → partition likely full. Force-rotate and retry.
    if (attempt == 0 && !_energyRotate()) break;
  }
  Serial.println("[LFS] Append failed");
  return false;
}

static uint32_t energyLogCount() {
  if (!_lfsOk || !LittleFS.exists(ENERGY_FILE)) return 0;
  File f = LittleFS.open(ENERGY_FILE, "r");
  if (!f) return 0;
  uint32_t n = f.size() / sizeof(MinRecord);
  f.close();
  return n;
}

// Save prevBoxKwh[3] and prevBoxKvarh[3] to /prev_box.bin (24 bytes, fixed-size overwrite).
static void energySavePrevBox(const float kwh[3], const float kvarh[3]) {
  if (!_lfsOk || gPersistMode == 0) return;
  File f = LittleFS.open(PREV_BOX_FILE, "w");
  if (!f) { Serial.println("[LFS] prev_box write failed"); return; }
  f.write((const uint8_t*)kwh,   3 * sizeof(float));
  f.write((const uint8_t*)kvarh, 3 * sizeof(float));
  f.close();
}

// Load prevBoxKwh[3] and prevBoxKvarh[3] from /prev_box.bin.
// Returns true if the file existed and was read successfully.
static bool energyLoadPrevBox(float kwh[3], float kvarh[3]) {
  if (!_lfsOk || !LittleFS.exists(PREV_BOX_FILE)) return false;
  File f = LittleFS.open(PREV_BOX_FILE, "r");
  if (!f || f.size() < 6 * (int)sizeof(float)) { if (f) f.close(); return false; }
  bool ok = (f.read((uint8_t*)kwh,   3 * sizeof(float)) == 3 * sizeof(float)) &&
            (f.read((uint8_t*)kvarh, 3 * sizeof(float)) == 3 * sizeof(float));
  f.close();
  return ok;
}

// Read the last stored MinRecord into out. Returns true if a record was found.
static bool energyLoadLast(MinRecord& out) {
  if (!_lfsOk || !LittleFS.exists(ENERGY_FILE)) return false;
  File f = LittleFS.open(ENERGY_FILE, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz < sizeof(MinRecord)) { f.close(); return false; }
  f.seek(sz - sizeof(MinRecord));
  bool ok = (f.read((uint8_t*)&out, sizeof(MinRecord)) == sizeof(MinRecord));
  f.close();
  return ok;
}
