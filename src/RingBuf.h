#pragma once
#include <Arduino.h>

// PSRAM-backed circular ring buffer — runtime capacity.
// age=0 → newest entry, age=cnt-1 → oldest.
//
// NOT ISR-safe: push()/get() mutate head/cnt non-atomically. The energy rings
// are only touched from loop() (after a parse cycle), never from an ISR, so no
// locking is needed. Do not call push() from an interrupt without adding one.

// ── per-board capacity constants ──────────────────────────────────────────────
// Fallback for legacy consumers only. Sketches that include <BoardProfile.h>
// (before this header) get their caps from the asserted board profile instead;
// new code should do that rather than rely on the silent S3-Zero default below.
#ifndef SEC_CAP_LTE
#if defined(BOARD_DEVKITC1) || defined(BOARD_LILYGO_T7S3)
  // 16 MB flash / 8 MB PSRAM
  #define SEC_CAP_LTE  14400u   // 4 h  at 1 rec/s
  #define MIN_CAP_LTE  43200u   // 30 d at 1 rec/min
  #define SEC_CAP_ADF  86400u   // 24 h at 1 rec/s
  #define MIN_CAP_ADF   1440u   // 24 h at 1 rec/min
#else
  // ESP32-S3-Zero: 4 MB flash / 2 MB PSRAM (default)
  #define SEC_CAP_LTE   7200u   // 2 h   at 1 rec/s
  #define MIN_CAP_LTE  10080u   // 7 d   at 1 rec/min
  #define SEC_CAP_ADF  16200u   // 4.5 h at 1 rec/s
  #define MIN_CAP_ADF    270u   // 4.5 h at 1 rec/min
#endif
#endif // SEC_CAP_LTE

template<typename T>
struct RingBuf {
  typedef T rec_t;   // record type, for ring-generic templates (Snapshot.h)
  T*       buf     = nullptr;
  uint32_t cap     = 0;   // capacity actually obtained (0 = dead ring)
  uint32_t req_cap = 0;   // capacity last requested (cap < req_cap → downgraded)
  uint32_t head    = 0;
  uint32_t cnt     = 0;

  bool downgraded() const { return cap < req_cap; }

  // Try the requested capacity, then progressively smaller (¾, ½, ¼, ⅛) so a
  // tight or fragmented PSRAM yields a smaller live ring instead of a silent
  // dead one. Returns true if any capacity was obtained; check downgraded().
  bool init(uint32_t capacity) {
    req_cap = capacity;
    head = 0; cnt = 0;
    static const uint8_t num[5] = {8, 6, 4, 2, 1};   // eighths of the request
    for (int i = 0; i < 5; i++) {
      uint32_t c = (uint32_t)((uint64_t)capacity * num[i] / 8u);
      if (!c) break;
      buf = (T*)ps_malloc((size_t)c * sizeof(T));
      if (buf) {
        cap = c;
        memset(buf, 0, (size_t)c * sizeof(T));
        return true;
      }
    }
    cap = 0;
    return false;
  }

  // Mode-switch realloc. Allocates the new buffer first so a switch that
  // cannot fit keeps the previous ring intact (data included). Only if the
  // old and new rings cannot coexist is the old one released and init()'s
  // fallback ladder used — records are lost then (a mode switch clears them
  // anyway), but the ring never ends up dead: on total failure the previous
  // capacity is re-allocated empty. Returns true if cap == the requested cap.
  bool reinit(uint32_t newCap) {
    T* nb = (T*)ps_malloc((size_t)newCap * sizeof(T));
    if (nb) {
      if (buf) free(buf);
      buf = nb; cap = newCap; req_cap = newCap;
      memset(buf, 0, (size_t)newCap * sizeof(T));
      head = 0; cnt = 0;
      return true;
    }
    uint32_t oldCap = cap;
    if (buf) { free(buf); buf = nullptr; }
    cap = 0; head = 0; cnt = 0;
    if (init(newCap)) return !downgraded();
    if (oldCap) init(oldCap);   // restore a live ring at the old size (empty)
    req_cap = newCap;           // keep the downgrade visible either way
    return false;
  }

  void push(const T& r) {
    if (!buf) return;
    buf[head] = r;
    if (++head == cap) head = 0;
    if (cnt < cap) cnt++;
  }

  bool get(uint32_t age, T& out) const {
    if (!buf || age >= cnt) return false;
    out = buf[(head + cap - 1u - age) % cap];
    return true;
  }

  void clear() { head = 0; cnt = 0; }
};
