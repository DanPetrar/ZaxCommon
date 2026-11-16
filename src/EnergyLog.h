#pragma once
#include <LittleFS.h>
#include "Config.h"
#include "PersistBudget.h"

// Binary append log of MinRecord structs, preceded by an EnergyHeader.
// File: /energy.bin  =  EnergyHeader(8 B) + N * sizeof(MinRecord)
// Capped at ENERGY_MAX (1.2 MB); the runtime budget gEnergyMax caps it lower on
// small partitions.
//
// The header exists because this file used to have none, and was read as
// `f.size() / sizeof(MinRecord)`. A record-size change therefore did not fail —
// it MISPARSED silently, reinterpreting every field boundary. Nothing in the
// format could detect its own mismatch. `SnapHeader` (ZaxCommon/Snapshot.h) had
// solved the same problem years earlier; this mirrors it exactly rather than
// inventing a second convention.

static const char  ENERGY_FILE[]    = "/energy.bin";
static const char  ENERGY_TMP[]     = "/energy.tmp";
static const char  PREV_BOX_FILE[]  = "/prev_box.bin";  // 24 bytes: float[3] kwh + float[3] kvarh
static const size_t ENERGY_MAX      = 1200000UL;  // ~1.2 MB cap (~26 days)

// Mirrors SnapHeader's shape and size deliberately — same magic scheme, same
// data_ver/rec_size guard, same 8 bytes.
struct EnergyHeader {
  uint32_t magic;     // 0x5A415845 'ZAXE'
  uint8_t  data_ver;  // DATA_VERSION at write time
  uint8_t  rec_size;  // sizeof(MinRecord)
  uint16_t reserved;
};
static const uint32_t ENERGY_MAGIC = 0x5A415845UL;

static void _energyFillHeader(EnergyHeader& h) {
  h.magic    = ENERGY_MAGIC;
  h.data_ver = (uint8_t)DATA_VERSION;
  h.rec_size = (uint8_t)sizeof(MinRecord);
  h.reserved = 0;
}

// Separate flag from the global `lfsOk` used by ErrorLog/Snapshot. Both are set
// together via `lfsOk = energyLogInit();` in the sketch — keep that call intact,
// or the two views of "is LittleFS mounted" will diverge silently.
static bool _lfsOk = false;

// Records in a file of `sz` bytes. Guards the underflow: a file smaller than the
// header holds no records, and (sz - 8) must not be computed on it.
static uint32_t _energyRecs(size_t sz) {
  if (sz < sizeof(EnergyHeader)) return 0;
  return (uint32_t)((sz - sizeof(EnergyHeader)) / sizeof(MinRecord));
}

// Ensure /energy.bin exists and starts with a current header. Called before any
// append, because open("a") on a missing file would otherwise create a
// HEADERLESS one — reintroducing exactly the format this header exists to close.
static bool _energyEnsureFile() {
  if (LittleFS.exists(ENERGY_FILE)) return true;
  File f = LittleFS.open(ENERGY_FILE, "w");
  if (!f) return false;
  EnergyHeader h; _energyFillHeader(h);
  bool ok = (f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h));
  f.close();
  if (!ok) LittleFS.remove(ENERGY_FILE);
  return ok;
}

// One-time format migration, run from energyLogInit().
//
// PRESERVE WHERE POSSIBLE. A legacy headerless file whose size is a whole number
// of MinRecords still holds perfectly valid records — discarding it merely
// because it lacks a header we just invented would be gratuitous data loss, and
// losing recoverable data is the thing the buffer exists to prevent. Such a file
// is rewritten with a header and its records intact.
//
// A file that HAS a header but disagrees on rec_size or data_ver is a different
// case: its records cannot be reinterpreted, so it is discarded. That is the
// path a future MinRecord change takes.
static void _energyMigrate() {
  if (!LittleFS.exists(ENERGY_FILE)) return;
  File f = LittleFS.open(ENERGY_FILE, "r");
  if (!f) return;
  size_t sz = f.size();

  EnergyHeader h{};
  bool hasHdr = sz >= sizeof(EnergyHeader) &&
                f.read((uint8_t*)&h, sizeof(h)) == (int)sizeof(h) &&
                h.magic == ENERGY_MAGIC;

  if (hasHdr) {
    f.close();
    if (h.rec_size == (uint8_t)sizeof(MinRecord) && h.data_ver == (uint8_t)DATA_VERSION)
      return;                                   // current format, nothing to do
    Serial.printf("[LFS] energy.bin format changed (rec %u->%u, ver %u->%u) — discarded, "
                  "%u records lost\n",
                  (unsigned)h.rec_size, (unsigned)sizeof(MinRecord),
                  (unsigned)h.data_ver, (unsigned)DATA_VERSION, (unsigned)_energyRecs(sz));
    LittleFS.remove(ENERGY_FILE);
    _energyEnsureFile();
    return;
  }

  // No header. Legacy file if it is a whole number of records.
  if (sz == 0 || (sz % sizeof(MinRecord)) != 0) {
    f.close();
    Serial.printf("[LFS] energy.bin unrecognised (%u bytes, not a record multiple) — "
                  "discarded\n", (unsigned)sz);
    LittleFS.remove(ENERGY_FILE);
    _energyEnsureFile();
    return;
  }

  // Rewrite with a header, records preserved. tmp+rename so an interruption
  // leaves the original intact rather than a half-migrated file.
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) { f.close(); Serial.println("[LFS] energy.bin migration: no PSRAM, left as-is"); return; }
  f.seek(0);
  bool read_ok = (f.read(buf, sz) == (int)sz);
  f.close();

  bool ok = false;
  if (read_ok) {
    LittleFS.remove(ENERGY_TMP);
    File nf = LittleFS.open(ENERGY_TMP, "w");
    if (nf) {
      EnergyHeader nh; _energyFillHeader(nh);
      ok = (nf.write((const uint8_t*)&nh, sizeof(nh)) == sizeof(nh)) &&
           (nf.write(buf, sz) == sz);
      nf.close();
    }
  }
  free(buf);

  if (ok && LittleFS.rename(ENERGY_TMP, ENERGY_FILE)) {
    Serial.printf("[LFS] energy.bin migrated to headered format — %u records preserved\n",
                  (unsigned)(sz / sizeof(MinRecord)));
  } else {
    LittleFS.remove(ENERGY_TMP);
    Serial.println("[LFS] energy.bin migration failed — left in legacy format, will retry");
  }
}

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
  _energyMigrate();          // before any read below reports a count
  uint32_t recs = 0;
  if (LittleFS.exists(ENERGY_FILE)) {
    File f = LittleFS.open(ENERGY_FILE, "r");
    if (f) { recs = _energyRecs(f.size()); f.close(); }
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
  // Work in RECORDS, not raw bytes: the file now starts with a header, so byte
  // arithmetic that ignores it would slice mid-record.
  size_t sz    = f.size();
  uint32_t nrec = _energyRecs(sz);
  if (nrec < 2) { f.close(); return true; }      // nothing meaningful to halve
  uint32_t keepRec = nrec / 2;
  while (keepRec > 1 && sizeof(EnergyHeader) + (size_t)keepRec * sizeof(MinRecord) >= cap) keepRec /= 2;   // N14: a file far above a lowered cap gets under it in one rotation
  size_t skip  = sizeof(EnergyHeader) + (size_t)(nrec - keepRec) * sizeof(MinRecord);
  size_t keep  = (size_t)keepRec * sizeof(MinRecord);
  uint8_t* tmp = (uint8_t*)ps_malloc(keep);
  if (!tmp) { f.close(); return false; }
  f.seek(skip);
  bool read_ok = (f.read(tmp, keep) == (int)keep);
  f.close();

  // tmp+rename, not remove+write. The old code removed the file BEFORE writing,
  // so between those two calls the whole log existed only in PSRAM and a power
  // cut, brownout, panic or watchdog reset lost it (P8). Snapshot.h has always
  // done this correctly; rotation had not. Transient cost is the kept half
  // (~92 KB) against ~357 KB free at steady state — measured, not assumed.
  //
  // The header is written HERE too. Without it every rotation would produce a
  // headerless file, which _energyMigrate() would then rewrite on the next boot
  // — turning a ~2.3-day rotation into a churn of migrations.
  bool ok = false;
  if (read_ok) {
    LittleFS.remove(ENERGY_TMP);
    File nf = LittleFS.open(ENERGY_TMP, "w");
    if (nf) {
      EnergyHeader h; _energyFillHeader(h);
      ok = (nf.write((const uint8_t*)&h, sizeof(h)) == sizeof(h)) &&
           (nf.write(tmp, keep) == keep);
      nf.close();
    }
    if (ok) ok = LittleFS.rename(ENERGY_TMP, ENERGY_FILE);
    if (!ok) LittleFS.remove(ENERGY_TMP);       // leave the original untouched
  }
  free(tmp);
  Serial.printf("[LFS] Rotated energy.bin: kept %u records (%u bytes)%s\n",
                (unsigned)keepRec, (unsigned)keep, ok ? "" : " — FAILED, original kept");
  return ok;
}

// Append one MinRecord. Skipped (treated as success) when persistence is OFF or
// LittleFS is down. Returns true only on a confirmed full-size write — the
// caller feeds this to the persist guard. On a short write (partition full) it
// force-rotates once and retries before giving up.
static bool energyLogAppend(const MinRecord& rec) {
  if (!_lfsOk || gPersistMode == 0) return true;

  _energyRotate();
  if (!_energyEnsureFile()) { Serial.println("[LFS] energy.bin create failed"); return false; }

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
  uint32_t n = _energyRecs(f.size());
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
  if (_energyRecs(sz) == 0) { f.close(); return false; }
  f.seek(sz - sizeof(MinRecord));
  bool ok = (f.read((uint8_t*)&out, sizeof(MinRecord)) == sizeof(MinRecord));
  f.close();
  return ok;
}
