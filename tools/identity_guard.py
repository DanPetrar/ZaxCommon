#!/usr/bin/env python3
"""
identity_guard.py — unified firmware-identity guard (replaces the MAC catalog).

Reads the ZaxIdent descriptor (see ZaxCommon/src/ZaxIdent.h) embedded in a
firmware image AND on the connected device, and aborts a flash unless BOTH match
the {project, board, variant} the build script expects. Blocks wrong-board,
wrong-variant (BASE/SAT) and wrong-project flashes on the USB path — the same
descriptor the on-device OTA handler enforces.

Subcommands:
  scan   --bin FILE
         Print the ZaxIdent found in an image (diagnostic).
  ident  --port PORT [--host IP]
         Print the ZaxIdent currently on the device (HTTP /ident if --host,
         else esptool read-app-region + scan).
  check  --bin FILE --port PORT --expect-project N --expect-board N
         --expect-variant N [--host IP] [--first-flash]
         Validate image AND device against the expected tuple. Exit 0 = OK.

Names accepted for --expect-* may be ints or the canonical names below.
"""

import argparse, glob, json, struct, subprocess, sys, urllib.request

# ---- ZaxIdent layout (must mirror ZaxCommon/src/ZaxIdent.h — frozen) ----------
ZAXIDENT_MAGIC      = 0x5A584931        # 'ZXI1'
ZAXIDENT_SCHEMA_VER = 1
_FMT  = "<IBBBB16sH6s"                   # magic, schema, proj, board, variant, fw[16], dv, pad[6]
_SIZE = struct.calcsize(_FMT)            # = 32
assert _SIZE == 32, _SIZE

PROJECTS = {1: "EnergyCalibrator", 2: "ZaxModbus", 3: "ZaxEnergySurvey",
            10: "EmonESP-base", 11: "EmonESP-V001", 12: "EmonESP-V002"}
BOARDS   = {0: "lilygo", 1: "s3zero", 2: "devkitc1", 3: "classic_esp32"}
VARIANTS = {0: "default", 1: "base", 2: "sat"}

# Legacy descriptor used by the Zax family (EnergyCalibrator/ZaxModbus/ZaxEnergySurvey).
# Their OTA path already enforces project_id + hw_target; this lets the same guard read
# them on the USB path without changing that firmware.
ZAXOTAMETA_MAGIC = 0x5A415843            # 'ZAXC'
_OTA_FMT  = "<I12sBBHHB9s"               # magic, fw[12], hw_target, data_version, sec_sz, min_sz, project_id, pad[9]
_OTA_SIZE = struct.calcsize(_OTA_FMT)    # = 32
assert _OTA_SIZE == 32, _OTA_SIZE

ESPTOOL = next(iter(sorted(glob.glob(
    "/home/pi/.arduino15/packages/esp32/tools/esptool_py/*/esptool"), reverse=True)), None)
APP_OFFSET   = 0x10000      # app0 start under defaultffat / default schemes
SCAN_BYTES   = 0x80000      # 512 KB from app start — descriptor sits ~130 KB in


def _name(table, key):
    """Resolve an int or canonical name to its int id."""
    if key is None:
        return None
    s = str(key).strip().lower()
    if s.isdigit():
        return int(s)
    rev = {v.lower(): k for k, v in table.items()}
    if s not in rev:
        sys.exit(f"[guard] ERROR: unknown value '{key}' (known: {sorted(rev)})")
    return rev[s]


def parse_ident(blob, i):
    magic, schema, proj, board, variant, fw, dv, _ = struct.unpack_from(_FMT, blob, i)
    return {"magic": magic, "schema_ver": schema, "project_id": proj,
            "board_type": board, "variant": variant,
            "fw_version": fw.split(b"\0")[0].decode("latin1"), "data_version": dv,
            "_fmt": "zaxident"}


def _otameta_board(project_id, hw_target):
    """Map a ZaxOtaMeta hw_target to the canonical board_type (mapping was per-project).
       ZaxModbus(2)/ZaxEnergySurvey(3): hw_target 1=lilygo, 0=s3zero.
       EnergyCalibrator(1) used the inverse — retired, not supported here."""
    if project_id in (2, 3):
        return 0 if hw_target == 1 else 1     # 0=lilygo, 1=s3zero
    return None


def parse_otameta(blob, i):
    magic, fw, hw, dv, secsz, minsz, proj, _ = struct.unpack_from(_OTA_FMT, blob, i)
    return {"magic": magic, "schema_ver": None, "project_id": proj,
            "board_type": _otameta_board(proj, hw), "hw_target": hw, "variant": 0,
            "fw_version": fw.split(b"\0")[0].decode("latin1"), "data_version": dv,
            "sec_rec_size": secsz, "min_rec_size": minsz, "_fmt": "zaxotameta"}


def scan_blob(blob):
    """Return the first valid identity descriptor (ZaxIdent or legacy ZaxOtaMeta), or None."""
    # ZaxIdent (EmonESP family + future unified)
    needle = struct.pack("<I", ZAXIDENT_MAGIC)
    off = 0
    while True:
        i = blob.find(needle, off)
        if i < 0 or i + _SIZE > len(blob):
            break
        d = parse_ident(blob, i)
        if d["schema_ver"] == ZAXIDENT_SCHEMA_VER:    # filter coincidental magic
            d["_offset"] = i
            return d
        off = i + 1
    # ZaxOtaMeta (legacy Zax family)
    needle = struct.pack("<I", ZAXOTAMETA_MAGIC)
    off = 0
    while True:
        i = blob.find(needle, off)
        if i < 0 or i + _OTA_SIZE > len(blob):
            break
        d = parse_otameta(blob, i)
        # sanity-gate against a coincidental magic: plausible record sizes + known project
        if 0 < d["sec_rec_size"] < 1024 and 0 < d["min_rec_size"] < 1024 \
           and d["project_id"] in PROJECTS and d["board_type"] is not None:
            d["_offset"] = i
            return d
        off = i + 1
    return None


def fmt(d):
    return (f"project={d['project_id']}({PROJECTS.get(d['project_id'],'?')}) "
            f"board={d['board_type']}({BOARDS.get(d['board_type'],'?')}) "
            f"variant={d['variant']}({VARIANTS.get(d['variant'],'?')}) "
            f"fw={d['fw_version']}")


def ident_from_bin(path):
    with open(path, "rb") as f:
        d = scan_blob(f.read())
    if not d:
        sys.exit(f"[guard] ERROR: no ZaxIdent descriptor found in {path}")
    return d


def ident_from_http(host):
    url = f"http://{host}/ident"
    with urllib.request.urlopen(url, timeout=6) as r:
        j = json.load(r)
    return {"project_id": j["project_id"], "board_type": j["board_type"],
            "variant": j["variant"], "schema_ver": j["schema_ver"],
            "fw_version": j["fw_version"], "magic": int(j["magic"], 16),
            "data_version": j.get("data_version", 0), "_via": "http"}


def ident_from_device_flash(port):
    if not ESPTOOL:
        sys.exit("[guard] ERROR: bundled esptool not found")
    tmp = "/tmp/_idguard_app.bin"
    r = subprocess.run([ESPTOOL, "--port", port, "--baud", "460800", "read_flash",
                        hex(APP_OFFSET), hex(SCAN_BYTES), tmp],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        sys.exit(f"[guard] ERROR: esptool read_flash failed\n{(r.stdout + r.stderr)[-500:]}")
    with open(tmp, "rb") as f:
        d = scan_blob(f.read())
    if d:
        d["_via"] = "flash"
    return d


def device_ident(port, host):
    if host:
        try:
            return ident_from_http(host)
        except Exception as e:
            print(f"[guard] /ident over HTTP failed ({e}); falling back to esptool")
    return ident_from_device_flash(port)


def cmd_scan(a):
    print("[guard] image:", fmt(ident_from_bin(a.bin)))


def cmd_ident(a):
    d = device_ident(a.port, a.host)
    if not d:
        print("[guard] device: no ZaxIdent descriptor (blank / pre-identity firmware)")
        return
    print(f"[guard] device ({d.get('_via','?')}):", fmt(d))


def cmd_check(a):
    exp_p = _name(PROJECTS, a.expect_project)
    exp_b = _name(BOARDS,   a.expect_board)
    exp_v = _name(VARIANTS, a.expect_variant)
    exp = f"project={exp_p}({PROJECTS.get(exp_p,'?')}) board={exp_b}({BOARDS.get(exp_b,'?')}) variant={exp_v}({VARIANTS.get(exp_v,'?')})"

    # 1. Image must match the expected tuple.
    img = ident_from_bin(a.bin)
    if (img["project_id"], img["board_type"], img["variant"]) != (exp_p, exp_b, exp_v):
        print(f"[guard] ABORT: image identity != expected\n  expected: {exp}\n  image   : {fmt(img)}")
        sys.exit(1)
    if img.get("_fmt") == "zaxident" and img["schema_ver"] != ZAXIDENT_SCHEMA_VER:
        print(f"[guard] ABORT: image schema_ver {img['schema_ver']} != {ZAXIDENT_SCHEMA_VER}")
        sys.exit(1)

    # 2. Device must be the same kind of unit (or a permitted first flash).
    dev = device_ident(a.port, a.host)
    if not dev:
        if a.first_flash:
            print(f"[guard] OK (first-flash): blank/old device; image {fmt(img)}")
            return
        print("[guard] ABORT: no identity descriptor on device (blank / legacy pre-identity "
              "firmware). Pass --first-flash to allow flashing a fresh/legacy board.")
        sys.exit(1)
    if (dev["project_id"], dev["board_type"], dev["variant"]) != (exp_p, exp_b, exp_v):
        print(f"[guard] ABORT: device identity != expected (wrong unit on {a.port})\n"
              f"  expected: {exp}\n  device  : {fmt(dev)}")
        sys.exit(1)

    print(f"[guard] OK  image+device match  {exp}")


def main():
    ap = argparse.ArgumentParser(description="Unified firmware-identity guard (ZaxIdent).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan");  s.add_argument("--bin", required=True); s.set_defaults(fn=cmd_scan)
    i = sub.add_parser("ident"); i.add_argument("--port", required=True); i.add_argument("--host"); i.set_defaults(fn=cmd_ident)
    c = sub.add_parser("check")
    c.add_argument("--bin", required=True); c.add_argument("--port", required=True)
    c.add_argument("--expect-project", required=True); c.add_argument("--expect-board", required=True)
    c.add_argument("--expect-variant", required=True); c.add_argument("--host")
    c.add_argument("--first-flash", action="store_true"); c.set_defaults(fn=cmd_check)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
