#pragma once
#include <LittleFS.h>
#include "RingBuf.h"
#include "Config.h"
#include "ErrorLog.h"
#include "PersistBudget.h"

// Periodic atomic snapshot of PSRAM rings to LittleFS.
// Write flow: write to .tmp → remove(.bin) → rename(.tmp, .bin).
//   A write failure leaves the old .bin untouched. The one uncovered case is a
//   power loss in the remove→rename window: .bin is gone, only .tmp remains.
//   LittleFS rename won't replace an existing dst, hence the explicit remove.
// Restore flow: push records oldest-first into ring on boot.

#if TEST_MODE
#define SNAP_INTERVAL_S  120UL    // 2 min in test mode
#else
#define SNAP_INTERVAL_S  3600UL   // 1 hour in production
#endif

static const char SEC_SNAP_TMP[] = "/sec_snap.tmp";
static const char SEC_SNAP_BIN[] = "/sec_snap.bin";
static const char MIN_SNAP_TMP[] = "/min_snap.tmp";
static const char MIN_SNAP_BIN[] = "/min_snap.bin";

extern bool lfsOk;

// 8-byte header prepended to every snapshot file.
// If magic, data_ver, or rec_size mismatches on load, the file is discarded.
struct SnapHeader {
  uint32_t magic;     // 0x5A415853 ('ZAXS')
  uint8_t  data_ver;  // DATA_VERSION at write time
  uint8_t  rec_size;  // sizeof(T) — 76 for SecRecord, 32 for MinRecord
  uint16_t reserved;
};

// Write the newest cnt ring records (oldest→newest) into tmp.
// Removes a partial tmp on failure.
// Ring = RingBuf<T> or SegRingBuf<T> — anything with rec_t/get/push/cnt.
template<typename Ring>
static bool _snapTmpWrite(Ring& ring, const char* tmp, uint32_t cnt) {
  typedef typename Ring::rec_t T;
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;

  SnapHeader hdr = { 0x5A415853UL, DATA_VERSION, (uint8_t)sizeof(T), 0 };
  bool ok = (f.write((const uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));

  for (uint32_t age = cnt; age-- > 0 && ok; ) {
    T rec;
    if (!ring.get(age, rec)) { ok = false; break; }
    if (f.write((const uint8_t*)&rec, sizeof(T)) != sizeof(T)) ok = false;
  }
  f.close();

  if (!ok) LittleFS.remove(tmp);
  return ok;
}

// Write ring contents (oldest→newest) to tmp, then rename to dst.
// Returns true on success; leaves dst untouched on any failure — except the
// tight-space retry below, which drops dst first.
// max_bytes bounds the file: when set, only the newest records that fit are
// written (the rest — oldest — are dropped). 0 = unbounded (whole ring).
template<typename Ring>
static bool _snapWrite(Ring& ring, const char* tmp, const char* dst, size_t max_bytes) {
  typedef typename Ring::rec_t T;
  uint32_t cnt = ring.cnt;
  if (cnt == 0) return true;

  // Bound to the newest records that fit the budget (header + N*sizeof(T)).
  if (max_bytes > sizeof(SnapHeader)) {
    uint32_t fit = (uint32_t)((max_bytes - sizeof(SnapHeader)) / sizeof(T));
    if (cnt > fit) cnt = fit;
  }
  if (cnt == 0) return true;

  if (!_snapTmpWrite(ring, tmp, cnt)) {
    // Not enough space for old + new to coexist (near-full partition, or a
    // unit still carrying a pre-v1.0.7 oversized snapshot next to the new
    // budget). Drop the old snapshot and retry once — atomicity is sacrificed
    // only in this degraded case, and only this snapshot type is at risk.
    if (!LittleFS.exists(dst)) return false;
    LittleFS.remove(dst);
    errorLog("WARN", "Snapshot tight on space — old dropped, retrying");
    if (!_snapTmpWrite(ring, tmp, cnt)) return false;
  }
  LittleFS.remove(dst);
  return LittleFS.rename(tmp, dst);
}

// Read snapshot file and push records (oldest-first) into ring.
// If the snapshot has more records than ring.cap, the oldest are naturally overwritten.
template<typename Ring>
static uint32_t _snapRead(Ring& ring, const char* path) {
  typedef typename Ring::rec_t T;
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  SnapHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic    != 0x5A415853UL ||
      hdr.data_ver != DATA_VERSION  ||
      hdr.rec_size != (uint8_t)sizeof(T)) {
    f.close();
    LittleFS.remove(path);
    errorLog("WARN", "Snap header mismatch — discarded");
    return 0;
  }

  uint32_t n = 0;
  T rec;
  while (f.read((uint8_t*)&rec, sizeof(T)) == sizeof(T)) {
    ring.push(rec);
    n++;
  }
  f.close();
  return n;
}

// Save both rings to LittleFS. Skipped (treated as success) if LittleFS is not
// mounted or persistence is OFF. Returns true only when both writes succeed —
// the caller feeds this to the persist guard. Snapshots are bounded by
// gSnapSecMax/gSnapMinMax (0 = unbounded) so the write always fits the partition.
template<typename SecRing, typename MinRing>
static bool snapshotSave(SecRing& secBuf, MinRing& minBuf) {
  if (!lfsOk || gPersistMode == 0) return true;
  uint32_t t0 = millis();
  bool ok1 = _snapWrite(secBuf, SEC_SNAP_TMP, SEC_SNAP_BIN, gSnapSecMax);
  bool ok2 = _snapWrite(minBuf, MIN_SNAP_TMP, MIN_SNAP_BIN, gSnapMinMax);
  if (!ok1) errorLog("ERROR", "Snapshot write failed: sec");
  if (!ok2) errorLog("ERROR", "Snapshot write failed: min");
  Serial.printf("[SNAP] sec=%s (%u rec)  min=%s (%u rec)  %u ms\n",
                ok1 ? "ok" : "FAIL", (unsigned)secBuf.cnt,
                ok2 ? "ok" : "FAIL", (unsigned)minBuf.cnt,
                (unsigned)(millis() - t0));
  return ok1 && ok2;
}

// Restore both rings from LittleFS snapshots. Call once after ring init + LittleFS mount.
template<typename SecRing, typename MinRing>
static void snapshotLoad(SecRing& secBuf, MinRing& minBuf) {
  if (!lfsOk) return;
  uint32_t n1 = _snapRead(secBuf, SEC_SNAP_BIN);
  uint32_t n2 = _snapRead(minBuf, MIN_SNAP_BIN);
  if (n1 || n2)
    Serial.printf("[SNAP] Restored sec=%u  min=%u records from flash\n", n1, n2);
}
