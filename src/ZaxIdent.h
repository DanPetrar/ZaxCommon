#pragma once
#include <stdint.h>

// ZaxIdent — unified firmware identity descriptor (the immutable contract).
//
// One fixed-size, magic-scannable struct embedded in EVERY project's firmware
// image. It is validated on BOTH flash paths — USB pre-flash guard and OTA — to
// block the three mis-flash classes:
//   A. wrong board type within a project   (LilyGO vs S3-Zero vs DevKitC1 ...)
//   B. wrong destination of the same HW    (BASE vs SAT)
//   C. wrong project entirely              (cross-project flash/OTA)
//
// IMMUTABLE — like the 4-byte sensor frame: once shipped, the magic, the field
// layout, the 32-byte size, and the project_id / board_type / variant
// enumerations must NEVER change or be renumbered. Extend only by APPENDING new
// enum values and consuming reserved pad bytes; bump `schema_ver` if the layout
// ever has to grow (older readers then reject the higher schema, by design).

#define ZAXIDENT_MAGIC       0x5A584931UL   // 'ZXI1' — distinct from ZaxOtaMeta (0x5A415843)
#define ZAXIDENT_SCHEMA_VER  1

// ---- project_id — globally unique per firmware project. APPEND ONLY. ----------
enum : uint8_t {
  ZAXID_PROJ_ENERGYCALIBRATOR = 1,
  ZAXID_PROJ_ZAXMODBUS        = 2,
  ZAXID_PROJ_ZAXENERGYSURVEY  = 3,
  ZAXID_PROJ_EMONESP_BASE     = 10,   // EmonESP_MultiIO (base lineage)
  ZAXID_PROJ_EMONESP_V001     = 11,   // EmonESP_MultiIO-V001
  ZAXID_PROJ_EMONESP_V002     = 12,   // EmonESP_MultiIO-V002
};

// ---- board_type — ONE canonical mapping for all projects. APPEND ONLY. --------
// (Replaces the old per-project `hw_target`, whose mapping was inconsistent.)
enum : uint8_t {
  ZAXID_BOARD_LILYGO        = 0,   // LilyGO T7 S3 (16 MB)
  ZAXID_BOARD_S3ZERO        = 1,   // Waveshare ESP32-S3-Zero (4 MB)
  ZAXID_BOARD_DEVKITC1      = 2,   // ESP32-S3 DevKitC-1
  ZAXID_BOARD_CLASSIC_ESP32 = 3,   // classic ESP32-D0WD (e.g. EmonESP slaves, 16 MB)
};

// ---- variant — destination/feature of the same hardware. APPEND ONLY. ---------
enum : uint8_t {
  ZAXID_VARIANT_DEFAULT = 0,   // single-build project
  ZAXID_VARIANT_BASE    = 1,   // EmonESP BASE
  ZAXID_VARIANT_SAT     = 2,   // EmonESP SAT
};

struct __attribute__((packed)) ZaxIdent {
  uint32_t magic;          // ZAXIDENT_MAGIC
  uint8_t  schema_ver;     // ZAXIDENT_SCHEMA_VER
  uint8_t  project_id;     // ZAXID_PROJ_*
  uint8_t  board_type;     // ZAXID_BOARD_*
  uint8_t  variant;        // ZAXID_VARIANT_*
  char     fw_version[16]; // human-readable, e.g. "2.0.2" (informational; not compared)
  uint16_t data_version;   // payload/record-format compat (0 if N/A)
  uint8_t  _pad[6];        // reserved — keeps the struct at 32 bytes
};

static_assert(sizeof(ZaxIdent) == 32,
              "ZaxIdent must stay exactly 32 bytes — frozen contract (OTA scan + flash-guard read depend on it)");

// The guard/OTA validator compares these four fields; fw_version/data_version are
// informational and intentionally NOT part of the identity match.
//   matches := magic==ZAXIDENT_MAGIC && schema_ver==ZAXIDENT_SCHEMA_VER
//              && project_id==expected && board_type==expected && variant==expected
