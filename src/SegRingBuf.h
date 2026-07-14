#pragma once
// SegRingBuf — PSRAM ring buffer backed by a circular queue of fixed-size
// segments instead of one contiguous allocation (buffer-redesign Phase 3,
// ZaxModbus Doc/segring-design-spec.md). Capacity is limited by *total* free
// PSRAM, not by the largest contiguous block, and resize() grows/shrinks a
// live ring without clearing it.
//
// Interface parity with RingBuf<T>: cap / req_cap / cnt fields, push() /
// get(age) / clear() / downgraded(), age 0 = newest. Consumers that only use
// those compile unchanged. RingBuf<T> itself is kept for legacy consumers.
//
// Semantics difference vs RingBuf (approved D5): when the ring is full, a
// push that fills the newest segment recycles the OLDEST whole segment —
// cnt drops by one segment of records (sawtooth) instead of by 1. Invariant:
// every data segment except the newest holds exactly rps records.
//
// NOT ISR-safe, no locking: Core 1 (loop) is the sole ring owner, same
// contract as RingBuf.

#if defined(ZAX_SEGRING_HOST)
  #include <cstdint>
  #include <cstring>
#else
  #include <Arduino.h>
  #include <esp_heap_caps.h>
#endif

// Allocation hooks — host tests override these to inject failures.
#ifndef ZAX_SEG_ALLOC
  #define ZAX_SEG_ALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
  #define ZAX_SEG_FREE(p)   heap_caps_free(p)
#endif

template<typename T, uint16_t MAXSEG = 128>
struct SegRingBuf {
  typedef T rec_t;

  T*       seg[MAXSEG] = {};   // allocated segment pointers, seg[0..segAlloc)
  uint32_t rps       = 0;      // records per segment
  uint32_t seg_bytes = 0;
  uint16_t segAlloc  = 0;      // segments actually allocated
  uint16_t segTarget = 0;      // segments last requested
  uint16_t firstSeg  = 0;      // index in seg[] of the oldest data segment
  uint16_t usedSegs  = 0;      // segments currently holding data
  uint32_t fill      = 0;      // records in the newest data segment (1..rps once pushed)
  uint32_t cap       = 0;      // segAlloc * rps (capacity actually obtained)
  uint32_t req_cap   = 0;      // segTarget * rps
  uint32_t cnt       = 0;      // records held

  bool downgraded() const { return cap < req_cap; }

  // Allocate up to nSegs segments of segBytes each. Partial success leaves a
  // smaller live ring (downgraded() true); only total failure returns false.
  bool init(uint32_t segBytes, uint16_t nSegs) {
    seg_bytes = segBytes;
    rps = segBytes / (uint32_t)sizeof(T);
    if (nSegs > MAXSEG) nSegs = MAXSEG;
    segTarget = nSegs;
    req_cap   = (uint32_t)nSegs * rps;
    firstSeg = usedSegs = 0; fill = 0; cnt = 0;
    segAlloc = 0;
    for (uint16_t i = 0; i < nSegs; i++) {
      T* p = (T*)ZAX_SEG_ALLOC(seg_bytes);
      if (!p) break;                       // same-size allocs won't fare better
      memset(p, 0, seg_bytes);
      seg[segAlloc++] = p;
    }
    cap = (uint32_t)segAlloc * rps;
    return segAlloc > 0;
  }

  void push(const T& r) {
    if (!segAlloc) return;
    if (!usedSegs) { usedSegs = 1; fill = 0; }
    else if (fill == rps) {                // newest segment full → advance
      if (usedSegs < segAlloc) usedSegs++;
      else {                               // recycle the oldest segment
        firstSeg = (uint16_t)((firstSeg + 1u) % segAlloc);
        cnt -= rps;
      }
      fill = 0;
    }
    seg[((uint32_t)firstSeg + usedSegs - 1u) % segAlloc][fill++] = r;
    cnt++;
  }

  bool get(uint32_t age, T& out) const {
    if (age >= cnt) return false;
    uint32_t back, off;                    // segments back from newest, offset
    if (age < fill) { back = 0; off = fill - 1u - age; }
    else {
      uint32_t k = age - fill;
      back = k / rps + 1u;
      off  = rps - 1u - (k % rps);
    }
    const T* s = seg[((uint32_t)firstSeg + usedSegs - 1u - back) % segAlloc];
    out = s[off];
    return true;
  }

  void clear() { firstSeg = usedSegs = 0; fill = 0; cnt = 0; }

  // Live resize to nSegs segments (min 1). Grow keeps every record; shrink
  // frees empty segments first, then drops the OLDEST data segments only.
  // Partial grow (alloc failure) leaves the ring at whatever it reached —
  // data intact, downgraded() true. Returns true iff the full target was met.
  bool resize(uint16_t nSegs) {
    if (nSegs < 1) nSegs = 1;
    if (nSegs > MAXSEG) nSegs = MAXSEG;
    segTarget = nSegs;
    req_cap   = (uint32_t)nSegs * rps;
    _normalize();                          // data queue → seg[0..usedSegs)

    while (segAlloc < nSegs) {             // grow
      T* p = (T*)ZAX_SEG_ALLOC(seg_bytes);
      if (!p) break;
      memset(p, 0, seg_bytes);
      seg[segAlloc++] = p;
    }
    while (segAlloc > nSegs && segAlloc > usedSegs) {   // free empties
      ZAX_SEG_FREE(seg[--segAlloc]);
      seg[segAlloc] = nullptr;
    }
    while (segAlloc > nSegs) {             // drop oldest data segments
      uint32_t drop = (usedSegs == 1) ? fill : rps;
      ZAX_SEG_FREE(seg[0]);
      for (uint16_t i = 1; i < segAlloc; i++) seg[i - 1] = seg[i];
      seg[--segAlloc] = nullptr;
      usedSegs--;
      cnt -= drop;
      if (!usedSegs) fill = 0;
    }
    cap = (uint32_t)segAlloc * rps;
    return segAlloc == segTarget;
  }

private:
  // Rotate the pointer map so the oldest data segment sits at seg[0]. Only
  // the map is permuted — segment contents don't move. Makes the queue
  // non-wrapping so segments can be appended/removed at the array ends.
  void _normalize() {
    if (!firstSeg || !segAlloc) { firstSeg = 0; return; }
    T* tmp[MAXSEG];
    for (uint16_t i = 0; i < segAlloc; i++)
      tmp[i] = seg[((uint32_t)firstSeg + i) % segAlloc];
    memcpy(seg, tmp, (size_t)segAlloc * sizeof(T*));
    firstSeg = 0;
  }
};
