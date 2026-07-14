#pragma once
#include "ErrorLog.h"
#include "Config.h"

// ── Fault bit positions (match fault_mask bits in ZaxConfig) ─────────────────
#define BIT_COMM_LOST    0
#define BIT_VOLT_ZERO    1
#define BIT_VOLT_UNDER   2
#define BIT_VOLT_OVER    3
#define BIT_CURR_OVER    4
#define BIT_CURR_ZERO    5
#define BIT_PF_LOW       6
#define BIT_FREQ         7
#define BIT_CHAN_MISS     8
#define BIT_KWH_ROLLBACK 9

// ── Persistent fault state — zero-initialised at boot = all clear ─────────────
struct FaultState {
  bool    commLost;
  uint8_t voltState[3];  // 0=ok 1=zero 2=under 3=over
  bool    currOver[3];
  bool    currZero[3];   // zero current while voltage is present
  bool    pfLow[3];
  bool    freqFault;
  // Onset timestamps — set when fault first fires, cleared on recovery
  uint32_t onsetCommLost;
  uint32_t onsetVolt[3];
  uint32_t onsetCurrOver[3];
  uint32_t onsetCurrZero[3];
  uint32_t onsetPfLow[3];
  uint32_t onsetFreq;
  // Shared repeat timer
  uint32_t lastRepeatMs;
};

extern FaultState faults;
extern bool       gFaultChanged;  // set true on any FaultState change → triggers mqttPublishFaults

static const char FAULT_CH[3][2] = {"R", "S", "T"};

inline bool faultEnabled(uint8_t bit, const ZaxConfig& cfg) {
  return (cfg.fault_mask >> bit) & 1;
}

// Forward declaration — defined in ZaxMonitor.ino after MQTT setup.
void mqttFaultEvent(const char* level, const char* code, int ch_idx, const char* msg);

// ── Elapsed minutes helper ────────────────────────────────────────────────────
static uint32_t elapsedMin(uint32_t onsetMs) {
  uint32_t ms = millis() - onsetMs;
  uint32_t m  = ms / 60000UL;
  return m > 0 ? m : 1;
}

// ── Call every loop() iteration (skip in demo mode) ──────────────────────────

static void faultCheckComm(uint32_t lastDataMs, const ZaxConfig& cfg) {
  if (!faultEnabled(BIT_COMM_LOST, cfg)) {
    // Disabled ⇒ no detection at all; force-clear a stale latch silently so it
    // can't freeze latched or emit recovery messages (Unit_B log-flood bug)
    if (faults.commLost) { faults.commLost = false; faults.onsetCommLost = 0; gFaultChanged = true; }
    return;
  }
  uint32_t tmo  = (uint32_t)cfg.comm_timeout_s * 1000UL;
  bool     lost = (millis() - lastDataMs) >= tmo;
  if (lost && !faults.commLost) {
    faults.commLost      = true;
    faults.onsetCommLost = millis();
    gFaultChanged        = true;
    char msg[64];
    snprintf(msg, sizeof(msg), "Box comm lost — no data for %us", (unsigned)cfg.comm_timeout_s);
    errorLog("ERROR", msg);
    mqttFaultEvent("ERROR", "comm_lost", -1, msg);
  } else if (!lost && faults.commLost) {
    faults.commLost      = false;
    faults.onsetCommLost = 0;
    gFaultChanged        = true;
    errorLog("INFO", "Box comm restored");
    mqttFaultEvent("INFO", "comm_ok", -1, "Box comm restored");
  }
}

// ── Call after a complete parse cycle (ch==2 successfully parsed) ─────────────

static void faultCheckSec(const SecRecord& rec, const ZaxConfig& cfg) {
  for (int i = 0; i < 3; i++) {
    if (!(cfg.ch_mask & (1 << i))) continue;  // skip disabled channels
    float v     = rec.v[i];
    float a     = rec.a[i];
    float absPf = fabsf(rec.pf[i]);
    float hz    = rec.hz[i];

    // ── Voltage ──────────────────────────────────────────────────────────────
    uint8_t newVolt;
    if      (v < 1.0f)          newVolt = 1;
    else if (v < cfg.volt_min)  newVolt = 2;
    else if (v > cfg.volt_max)  newVolt = 3;
    else                        newVolt = 0;

    static const uint8_t voltSubBit[4] = {0xFF, BIT_VOLT_ZERO, BIT_VOLT_UNDER, BIT_VOLT_OVER};

    // Disabled sub-type ⇒ no detection at all: treat the reading as OK
    if (newVolt != 0 && !faultEnabled(voltSubBit[newVolt], cfg)) newVolt = 0;
    // A latched sub-type whose bit got disabled is cleared silently — else it
    // would freeze latched forever or flood recovery messages (Unit_B bug)
    uint8_t curVolt = faults.voltState[i];
    if (curVolt != 0 && !faultEnabled(voltSubBit[curVolt], cfg)) {
      faults.voltState[i] = 0; faults.onsetVolt[i] = 0; gFaultChanged = true;
      curVolt = 0;
    }

    if (newVolt != curVolt) {
      char msg[64];
      const char* lvl;
      const char* code;
      if (newVolt == 0) {
        // Recovery text must mirror its onset: "restored" is for recovery-from-
        // absence; recovery-from-excess reads backwards, so over-V says "cleared"
        if (curVolt == 3)
          snprintf(msg, sizeof(msg), "Phase %s: overvoltage cleared %.1fV", FAULT_CH[i], v);
        else
          snprintf(msg, sizeof(msg), "Phase %s: voltage restored %.1fV", FAULT_CH[i], v);
        lvl = "INFO";  code = "volt_ok";
        faults.onsetVolt[i] = 0;
      } else if (newVolt == 1) {
        snprintf(msg, sizeof(msg), "Phase %s: zero voltage", FAULT_CH[i]);
        lvl = "ALERT"; code = "volt_zero";
        faults.onsetVolt[i] = millis();
      } else if (newVolt == 2) {
        snprintf(msg, sizeof(msg), "Phase %s: undervoltage %.1fV", FAULT_CH[i], v);
        lvl = "WARN";  code = "volt_under";
        faults.onsetVolt[i] = millis();
      } else {
        snprintf(msg, sizeof(msg), "Phase %s: overvoltage %.1fV", FAULT_CH[i], v);
        lvl = "ALERT"; code = "volt_over";
        faults.onsetVolt[i] = millis();
      }
      faults.voltState[i] = newVolt;
      gFaultChanged        = true;
      errorLog(lvl, msg);
      mqttFaultEvent(lvl, code, i, msg);
    }

    // ── Current (only meaningful when voltage is present) ────────────────────
    if (v > 1.0f) {
      if (!faultEnabled(BIT_CURR_OVER, cfg)) {
        // Disabled ⇒ no detection; force-clear a stale latch silently
        if (faults.currOver[i]) { faults.currOver[i] = false; faults.onsetCurrOver[i] = 0; gFaultChanged = true; }
      } else {
        bool over = (a > cfg.current_max);
        if (over && !faults.currOver[i]) {
          char msg[64];
          snprintf(msg, sizeof(msg), "Phase %s: overcurrent %.1fA", FAULT_CH[i], a);
          faults.currOver[i]      = true;
          faults.onsetCurrOver[i] = millis();
          gFaultChanged           = true;
          errorLog("ALERT", msg);
          mqttFaultEvent("ALERT", "curr_over", i, msg);
        } else if (!over && faults.currOver[i]) {
          char msg[64];
          snprintf(msg, sizeof(msg), "Phase %s: overcurrent cleared %.1fA", FAULT_CH[i], a);
          faults.currOver[i]      = false;
          faults.onsetCurrOver[i] = 0;
          gFaultChanged           = true;
          errorLog("INFO", msg);
          mqttFaultEvent("INFO", "curr_ok", i, msg);
        }
      }

      if (!faultEnabled(BIT_CURR_ZERO, cfg)) {
        if (faults.currZero[i]) { faults.currZero[i] = false; faults.onsetCurrZero[i] = 0; gFaultChanged = true; }
      } else {
        bool czero = (a < 0.05f);
        if (czero && !faults.currZero[i]) {
          char msg[64];
          snprintf(msg, sizeof(msg), "Phase %s: zero current, voltage=%.1fV", FAULT_CH[i], v);
          faults.onsetCurrZero[i] = millis();
          errorLog("WARN", msg);
          mqttFaultEvent("WARN", "curr_zero", i, msg);
        }
        if (!czero && faults.currZero[i]) faults.onsetCurrZero[i] = 0;
        if (czero != faults.currZero[i]) gFaultChanged = true;
        faults.currZero[i] = czero;
      }

      // ── Power factor (only with meaningful load) ──────────────────────────
      if (a > 0.1f) {
        if (!faultEnabled(BIT_PF_LOW, cfg)) {
          if (faults.pfLow[i]) { faults.pfLow[i] = false; faults.onsetPfLow[i] = 0; gFaultChanged = true; }
        } else {
          bool low = (absPf < cfg.pf_min);
          if (low && !faults.pfLow[i]) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Phase %s: low PF %.2f", FAULT_CH[i], absPf);
            faults.onsetPfLow[i] = millis();
            errorLog("WARN", msg);
            mqttFaultEvent("WARN", "pf_low", i, msg);
          }
          if (!low && faults.pfLow[i]) faults.onsetPfLow[i] = 0;
          if (low != faults.pfLow[i]) gFaultChanged = true;
          faults.pfLow[i] = low;
        }
      } else {
        if (faults.pfLow[i]) { gFaultChanged = true; faults.onsetPfLow[i] = 0; }
        faults.pfLow[i] = false;
      }
    } else {
      // No voltage — clear current faults silently
      if (faults.currOver[i] || faults.currZero[i] || faults.pfLow[i]) gFaultChanged = true;
      faults.currOver[i]      = false; faults.onsetCurrOver[i] = 0;
      faults.currZero[i]      = false; faults.onsetCurrZero[i] = 0;
      faults.pfLow[i]         = false; faults.onsetPfLow[i]    = 0;
    }

    // ── Frequency (R channel only — same grid source for all phases) ─────────
    if (i == 0 && hz > 1.0f) {
      if (!faultEnabled(BIT_FREQ, cfg)) {
        if (faults.freqFault) { faults.freqFault = false; faults.onsetFreq = 0; gFaultChanged = true; }
      } else {
        bool outBand = (hz < cfg.freq_min || hz > cfg.freq_max);
        if (outBand && !faults.freqFault) {
          char msg[48];
          snprintf(msg, sizeof(msg), "Freq out of band: %.2fHz", hz);
          faults.onsetFreq = millis();
          errorLog("WARN", msg);
          mqttFaultEvent("WARN", "freq", -1, msg);
        }
        if (!outBand && faults.freqFault) faults.onsetFreq = 0;
        if (outBand != faults.freqFault) gFaultChanged = true;
        faults.freqFault = outBand;
      }
    }
  }
}

// ── Call when ch==2 arrives — checks for missing R/S channels ────────────────

static void faultCheckCycle(bool* chanSeen, const ZaxConfig& cfg) {
  for (int i = 0; i < 2; i++) {
    if (!(cfg.ch_mask & (1 << i))) { chanSeen[i] = false; continue; }
    if (!chanSeen[i] && faultEnabled(BIT_CHAN_MISS, cfg)) {
      char msg[32];
      snprintf(msg, sizeof(msg), "Missing channel: %s", FAULT_CH[i]);
      errorLog("WARN", msg);
      mqttFaultEvent("WARN", "chan_miss", i, msg);  // transient — no FaultState change
    }
  }
  chanSeen[0] = chanSeen[1] = chanSeen[2] = false;
}

// ── Call from parse_min when kWh rollback detected ───────────────────────────

static void faultOnKwhRollback(int ch, const ZaxConfig& cfg) {
  if (!faultEnabled(BIT_KWH_ROLLBACK, cfg)) return;
  char msg[56];
  snprintf(msg, sizeof(msg), "Box power loss — kWh rolled back ch=%s", FAULT_CH[ch]);
  errorLog("ALERT", msg);
  mqttFaultEvent("ALERT", "kwh_rollback", ch, msg);  // transient — no FaultState change
}

// ── Periodic re-emit of active faults that have not been resolved ─────────────
// Call every loop() iteration (skip in demo mode). Timer is self-managed.

static void faultRepeatCheck(const ZaxConfig& cfg) {
  if (cfg.fault_repeat_min == 0) return;
  uint32_t interval = (uint32_t)cfg.fault_repeat_min * 60000UL;
  if (millis() - faults.lastRepeatMs < interval) return;
  faults.lastRepeatMs = millis();

  char msg[80];

  if (faults.commLost && faultEnabled(BIT_COMM_LOST, cfg)) {
    uint32_t m = elapsedMin(faults.onsetCommLost);
    snprintf(msg, sizeof(msg), "Box comm lost — not recovered (%um)", (unsigned)m);
    errorLog("ERROR", msg);
    mqttFaultEvent("ERROR", "comm_lost", -1, msg);
  }

  static const char* voltDesc[] = {"", "zero voltage", "undervoltage", "overvoltage"};
  static const char* voltCode[] = {"", "volt_zero", "volt_under", "volt_over"};
  static const char* voltLvl[]  = {"", "ALERT", "WARN", "ALERT"};
  static const uint8_t voltBit[] = {0, BIT_VOLT_ZERO, BIT_VOLT_UNDER, BIT_VOLT_OVER};

  for (int i = 0; i < 3; i++) {
    uint8_t vs = faults.voltState[i];
    if (vs != 0 && faultEnabled(voltBit[vs], cfg)) {
      uint32_t m = elapsedMin(faults.onsetVolt[i]);
      snprintf(msg, sizeof(msg), "Phase %s: %s — not recovered (%um)",
               FAULT_CH[i], voltDesc[vs], (unsigned)m);
      errorLog(voltLvl[vs], msg);
      mqttFaultEvent(voltLvl[vs], voltCode[vs], i, msg);
    }

    if (faults.currOver[i] && faultEnabled(BIT_CURR_OVER, cfg)) {
      uint32_t m = elapsedMin(faults.onsetCurrOver[i]);
      snprintf(msg, sizeof(msg), "Phase %s: overcurrent — not recovered (%um)",
               FAULT_CH[i], (unsigned)m);
      errorLog("ALERT", msg);
      mqttFaultEvent("ALERT", "curr_over", i, msg);
    }

    if (faults.currZero[i] && faultEnabled(BIT_CURR_ZERO, cfg)) {
      uint32_t m = elapsedMin(faults.onsetCurrZero[i]);
      snprintf(msg, sizeof(msg), "Phase %s: zero current — not recovered (%um)",
               FAULT_CH[i], (unsigned)m);
      errorLog("WARN", msg);
      mqttFaultEvent("WARN", "curr_zero", i, msg);
    }

    if (faults.pfLow[i] && faultEnabled(BIT_PF_LOW, cfg)) {
      uint32_t m = elapsedMin(faults.onsetPfLow[i]);
      snprintf(msg, sizeof(msg), "Phase %s: low PF — not recovered (%um)",
               FAULT_CH[i], (unsigned)m);
      errorLog("WARN", msg);
      mqttFaultEvent("WARN", "pf_low", i, msg);
    }
  }

  if (faults.freqFault && faultEnabled(BIT_FREQ, cfg)) {
    uint32_t m = elapsedMin(faults.onsetFreq);
    snprintf(msg, sizeof(msg), "Freq out of band — not recovered (%um)", (unsigned)m);
    errorLog("WARN", msg);
    mqttFaultEvent("WARN", "freq", -1, msg);
  }
}
