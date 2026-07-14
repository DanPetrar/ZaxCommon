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
// The S3-Zero PSRAM_* limits are re-derived from Phase-0 telemetry (Unit_B
// v1.0.7, 6 h uptime, WiFi+MQTT up: system PSRAM overhead ≈44 KB, usable pool
// ≈2,050 KB — Doc/segring-design-spec.md §3.1). LilyGO values remain estimates
// (compile-check only, no hardware in scope). The Phase-3 capacity manager
// sizes rings from *measured* free SPIRAM at boot; PSRAM_USABLE only caps it.
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
  #define PSRAM_USABLE      ZAX_KB(1900)  // measured usable ≈2,050 KB, ~7% safety
  #define PSRAM_MAX_ALLOC   ZAX_KB(768)   // informational only since Phase 3 —
                                          // segments (≤64 KB) make contiguity moot
  #define PSRAM_HEADROOM    ZAX_KB(128)   // ~3× the measured ≈44 KB system use
  #define OTA_SCAN_BYTES    ZAX_KB(512)   // OTA meta-scan WINDOW (streaming since
                                          // v1.0.6, no allocation — not in the budget)
  // Legacy fixed caps — no longer read by ZaxModbus (the v1.1.0 capacity
  // manager sizes SegRingBuf from measured free SPIRAM); kept so RingBuf.h's
  // #ifndef fallback stays overridden for any consumer including this profile.
  #define SEC_CAP_LTE   7200u             // 2 h at 1 rec/s
  #define MIN_CAP_LTE  10080u             // 7 d at 1 rec/min
  #define SEC_CAP_ADF  SEC_CAP_LTE
  #define MIN_CAP_ADF  MIN_CAP_LTE
  // Phase-3 segmented-ring parameters (Doc/segring-design-spec.md §2.2/§3):
  #define ZAX_SEC_SEG_BYTES ZAX_KB(64)    // 862 SecRecord (76 B) per segment
  #define ZAX_MIN_SEG_BYTES ZAX_KB(16)    // 512 MinRecord (32 B) per segment
  #define ZAX_MIN_SEGS_LTE  20u           // 10,240 recs ≈ 7.1 d (today's shape)
  #define ZAX_MIN_SEGS_ADF   4u           // 2,048 recs ≈ 34 h — sec gets the rest
#elif defined(BOARD_LILYGO_T7S3) || defined(BOARD_DEVKITC1)
  // LilyGO T7-S3 / DevKitC-1 N16R8 — ALWAYS 8 MB OPI PSRAM / 16 MB flash
  #define PSRAM_TOTAL_BYTES ZAX_KB(8192)
  #define PSRAM_USABLE      ZAX_KB(7600)
  #define PSRAM_MAX_ALLOC   ZAX_KB(6656)
  #define PSRAM_HEADROOM    ZAX_KB(256)
  #define OTA_SCAN_BYTES    ZAX_KB(512)
  #define SEC_CAP_LTE  14400u             // legacy fixed caps — see S3-Zero note
  #define MIN_CAP_LTE  43200u
  #define SEC_CAP_ADF  86400u
  #define MIN_CAP_ADF   1440u
  #define ZAX_SEC_SEG_BYTES ZAX_KB(64)
  #define ZAX_MIN_SEG_BYTES ZAX_KB(16)
  #define ZAX_MIN_SEGS_LTE  85u           // 43,520 recs ≈ 30 d
  #define ZAX_MIN_SEGS_ADF   4u
#else
  #error "BoardProfile.h requires an explicit board flag: build with -DBOARD_S3ZERO, -DBOARD_LILYGO_T7S3 or -DBOARD_DEVKITC1 (no silent default)."
#endif

// Compile-time PSRAM budget guard, Phase-3 shape: ring sizes are decided at
// runtime from measured free SPIRAM, so the static check is a FLOOR — the
// fixed min-ring share plus a minimum viable sec ring (4 segments) and the
// headroom must fit the usable pool, and each record type must fit its
// segment. Record types live in the sketch, so the sketch invokes this after
// defining them.
#define ZAX_PSRAM_BUDGET_ASSERT(SecT, MinT)                                          \
  static_assert(sizeof(SecT) <= ZAX_SEC_SEG_BYTES,                                   \
                "SecRecord larger than a sec segment");                              \
  static_assert(sizeof(MinT) <= ZAX_MIN_SEG_BYTES,                                   \
                "MinRecord larger than a min segment");                              \
  static_assert((uint64_t)ZAX_MIN_SEGS_LTE * ZAX_MIN_SEG_BYTES +                     \
                4u * ZAX_SEC_SEG_BYTES + PSRAM_HEADROOM <= PSRAM_USABLE,             \
                "LTE min share + 4 sec segments + headroom exceed usable PSRAM");    \
  static_assert((uint64_t)ZAX_MIN_SEGS_ADF * ZAX_MIN_SEG_BYTES +                     \
                4u * ZAX_SEC_SEG_BYTES + PSRAM_HEADROOM <= PSRAM_USABLE,             \
                "ADF min share + 4 sec segments + headroom exceed usable PSRAM");    \
  static_assert(PSRAM_USABLE <= PSRAM_TOTAL_BYTES,                                   \
                "usable PSRAM exceeds the physical total")

#endif // ZAX_BOARD_PROFILE_H
