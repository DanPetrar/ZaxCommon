// BoardProfile.h — per-board memory profile: the single source of truth for
// PSRAM limits, ring capacities and the OTA scan reserve.
//
// OPT-IN header. A sketch that includes it MUST be built with exactly one
// explicit board flag (-DBOARD_S3ZERO / -DBOARD_LILYGO_T7S3 / -DBOARD_DEVKITC1);
// there is no silent default — a missing flag once selected S3-Zero caps on a
// LilyGO build path and vice versa, and the un-summed budgets caused the
// Unit_B dead-ring incident (ZaxModbus Doc/psram-ring-safety-spec.md §1/§7).
// Legacy ZaxCommon consumers that do not include this header keep the
// RingBuf.h fallback capacities and are unaffected.
//
// The PSRAM_* limits are conservative placeholders bracketing measured
// behaviour (S3-Zero: a 534 KB ps_malloc works, 1.17 MB fails even at boot;
// 849 KB rings + 512 KB OTA buffer coexist). They are to be tightened from
// /api/sysinfo psram_free / psram_largest telemetry (buffer-redesign Phase 0)
// before the Phase-3 capacity manager relies on them.
//
// Macro include guard (not #pragma once) — same two-include-path reasoning as
// ZaxModbus Config.h.
#ifndef ZAX_BOARD_PROFILE_H
#define ZAX_BOARD_PROFILE_H
#include <Arduino.h>

#define ZAX_KB(x) ((uint32_t)(x) * 1024u)

#if defined(BOARD_S3ZERO)
  // Waveshare ESP32-S3-Zero — ALWAYS 2 MB QSPI PSRAM / 4 MB flash / 128 KB LittleFS
  #define PSRAM_TOTAL_BYTES ZAX_KB(2048)
  #define PSRAM_USABLE      ZAX_KB(1400)  // relocatable pool (proven ≥1.36 MB in use)
  #define PSRAM_MAX_ALLOC   ZAX_KB(768)   // single-block ceiling (contiguity limit)
  #define PSRAM_HEADROOM    ZAX_KB(16)
  #define OTA_SCAN_BYTES    ZAX_KB(512)   // OTA meta-scan WINDOW (leading bytes searched for
                                          // ZaxOtaMeta). Since fw v1.0.6 the scan is streaming —
                                          // NO allocation; kept in the budget assert as a
                                          // conservative margin until the Phase-3 re-budget.
  #define SEC_CAP_LTE   7200u             // 2 h at 1 rec/s
  #define MIN_CAP_LTE  10080u             // 7 d at 1 rec/min
  // ADF aliases LTE on the Zero (locked decision, buffer-redesign-plan.md §5):
  // 2 MB cannot give ADF a larger sec window than LTE while preserving the OTA
  // reserve. Real ADF on this board arrives with the Phase-3 segmented ring.
  #define SEC_CAP_ADF  SEC_CAP_LTE
  #define MIN_CAP_ADF  MIN_CAP_LTE
#elif defined(BOARD_LILYGO_T7S3) || defined(BOARD_DEVKITC1)
  // LilyGO T7-S3 / DevKitC-1 N16R8 — ALWAYS 8 MB OPI PSRAM / 16 MB flash
  #define PSRAM_TOTAL_BYTES ZAX_KB(8192)
  #define PSRAM_USABLE      ZAX_KB(7600)
  #define PSRAM_MAX_ALLOC   ZAX_KB(6656)
  #define PSRAM_HEADROOM    ZAX_KB(256)
  #define OTA_SCAN_BYTES    ZAX_KB(512)
  #define SEC_CAP_LTE  14400u             // 4 h  at 1 rec/s
  #define MIN_CAP_LTE  43200u             // 30 d at 1 rec/min
  #define SEC_CAP_ADF  86400u             // 24 h at 1 rec/s
  #define MIN_CAP_ADF   1440u             // 24 h at 1 rec/min
#else
  #error "BoardProfile.h requires an explicit board flag: build with -DBOARD_S3ZERO, -DBOARD_LILYGO_T7S3 or -DBOARD_DEVKITC1 (no silent default)."
#endif

// Compile-time PSRAM budget guard (split guard, buffer-redesign-plan.md §5):
// each ring must fit the single-block contiguity ceiling, and the whole set
// plus the OTA reserve and headroom must fit the usable pool. Record types
// live in the sketch, so the sketch invokes this after defining them.
#define ZAX_PSRAM_BUDGET_ASSERT(SecT, MinT)                                          \
  static_assert((uint64_t)SEC_CAP_LTE * sizeof(SecT) <= PSRAM_MAX_ALLOC,             \
                "LTE sec ring exceeds the single-allocation PSRAM ceiling");         \
  static_assert((uint64_t)MIN_CAP_LTE * sizeof(MinT) <= PSRAM_MAX_ALLOC,             \
                "LTE min ring exceeds the single-allocation PSRAM ceiling");         \
  static_assert((uint64_t)SEC_CAP_ADF * sizeof(SecT) <= PSRAM_MAX_ALLOC,             \
                "ADF sec ring exceeds the single-allocation PSRAM ceiling");         \
  static_assert((uint64_t)MIN_CAP_ADF * sizeof(MinT) <= PSRAM_MAX_ALLOC,             \
                "ADF min ring exceeds the single-allocation PSRAM ceiling");         \
  static_assert((uint64_t)SEC_CAP_LTE * sizeof(SecT) +                               \
                (uint64_t)MIN_CAP_LTE * sizeof(MinT) +                               \
                OTA_SCAN_BYTES + PSRAM_HEADROOM <= PSRAM_USABLE,                     \
                "LTE rings + OTA reserve + headroom exceed usable PSRAM");           \
  static_assert((uint64_t)SEC_CAP_ADF * sizeof(SecT) +                               \
                (uint64_t)MIN_CAP_ADF * sizeof(MinT) +                               \
                OTA_SCAN_BYTES + PSRAM_HEADROOM <= PSRAM_USABLE,                     \
                "ADF rings + OTA reserve + headroom exceed usable PSRAM")

#endif // ZAX_BOARD_PROFILE_H
