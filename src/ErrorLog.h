#pragma once
#include <LittleFS.h>
#include <time.h>
#include "PersistBudget.h"

// Rolling text error log on LittleFS.
// Format: "YYYY-MM-DD HH:MM:SS [LEVEL] message\n"
// If clock is not set, timestamps use "BOOT+Xs" format.
// Rotates at ERR_LOG_MAX: keeps the second half (newest entries), written to
// ERR_LOG_TMP and renamed over the log (N29, fw 1.2.34).

static const char   ERR_LOG_FILE[] = "/errors.log";
static const char   ERR_LOG_TMP[]  = "/errors.tmp";
static const size_t ERR_LOG_MAX    = 65536UL;   // 64 KB

extern bool lfsOk;

static int8_t _errTzOfs = 0;
static void errorLogSetTz(int8_t tz) { _errTzOfs = tz; }

static void _errRotate() {
  size_t cap = gErrLogMax ? gErrLogMax : ERR_LOG_MAX;
  File f = LittleFS.open(ERR_LOG_FILE, "r");
  if (!f) return;
  size_t sz = f.size();
  if (sz < cap) { f.close(); return; }

  // Seek to halfway, then forward to next newline for a clean line boundary
  f.seek(sz / 2);
  int c;
  while ((c = f.read()) != -1 && c != '\n') {}
  size_t startPos = f.position();
  size_t keepSz   = sz - startPos;

  uint8_t* buf = (uint8_t*)ps_malloc(keepSz);
  if (!buf)   buf = (uint8_t*)malloc(keepSz);
  if (!buf) { f.close(); return; }

  size_t got = f.read(buf, keepSz);
  f.close();
  if (got != keepSz) { free(buf); return; }      // short read: keep the log as it is

  // tmp + rename: the log is never gone, a restart leaves the old file or the new.
  LittleFS.remove(ERR_LOG_TMP);                  // stale tmp from an earlier restart
  File nf = LittleFS.open(ERR_LOG_TMP, "w");
  bool wrote = false;
  if (nf) { wrote = (nf.write(buf, keepSz) == keepSz); nf.close(); }
  if (wrote) {
    if (!LittleFS.rename(ERR_LOG_TMP, ERR_LOG_FILE)) LittleFS.remove(ERR_LOG_TMP);
  } else {
    // No room for old + new together: degraded path (remove + write), so the log
    // can not stay at the cap for ever. Same trade as Snapshot.h _snapWrite.
    LittleFS.remove(ERR_LOG_TMP);
    LittleFS.remove(ERR_LOG_FILE);
    File of = LittleFS.open(ERR_LOG_FILE, "w");
    if (of) { of.write(buf, keepSz); of.close(); }
  }
  free(buf);
}

// Flood suppression state — same message within 60 s is counted but not written.
// The count is flushed as "...suppressed xN" when a different message arrives
// or when the 60 s window expires on the next call with the same message.
static char     _errLastMsg[200] = "";
static uint32_t _errLastMs       = 0;
static uint16_t _errRepeatCount  = 0;

static void _errWriteLine(const char* line) {
  if (!lfsOk || gPersistMode == 0) return;   // OFF → Serial-only (logged by caller)
  File f = LittleFS.open(ERR_LOG_FILE, "a");
  if (!f) return;
  f.write((const uint8_t*)line, strlen(line));
  f.close();
}

static void errorLog(const char* level, const char* msg) {
  char ts[24];
  time_t now = time(nullptr);
  if (now > 1000000L) {
    time_t local = now + (time_t)_errTzOfs * 3600L;
    struct tm ti;
    gmtime_r(&local, &ti);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);
  } else {
    snprintf(ts, sizeof(ts), "BOOT+%lus", (unsigned long)(millis() / 1000));
  }

  // Flood suppression: same message within 60 s → count, no write
  if (strcmp(msg, _errLastMsg) == 0 && (millis() - _errLastMs) < 60000UL) {
    _errRepeatCount++;
    Serial.printf("[ERRLOG suppressed x%u] %s [%s] %s\n", _errRepeatCount, ts, level, msg);
    return;
  }

  // New message (or same message after 60 s gap) — flush pending count first
  if (_errRepeatCount > 0) {
    char supLine[80];
    snprintf(supLine, sizeof(supLine), "%s [INFO] ...suppressed x%u\n", ts, _errRepeatCount);
    Serial.printf("[ERRLOG] %s", supLine);
    _errWriteLine(supLine);
    _errRepeatCount = 0;
  }

  strncpy(_errLastMsg, msg, sizeof(_errLastMsg) - 1);
  _errLastMsg[sizeof(_errLastMsg) - 1] = '\0';
  _errLastMs = millis();

  char line[200];
  snprintf(line, sizeof(line), "%s [%s] %s\n", ts, level, msg);
  Serial.printf("[ERRLOG] %s", line);
  // _errRotate() intentionally NOT called here — moved to errorLogIdle() to
  // avoid blocking the UART RX path. Rotation runs every 5 min from loop().
  _errWriteLine(line);
}

// Call from loop() periodically (every ~5 min). Handles log rotation without
// blocking errorLog() on the critical UART receive path.
// Not with Onboard Storage off or in the power-fail STOPPED state (both set
// gPersistMode 0): a rotation is a flash write.
static void errorLogIdle() {
  if (lfsOk && gPersistMode != 0) _errRotate();
}

static void errorLogClear() {
  if (lfsOk) LittleFS.remove(ERR_LOG_FILE);
  _errLastMsg[0] = '\0';
  _errRepeatCount = 0;
}
