"""Build-integrity guard for the fork's custom-kernel envs.

WHY THIS EXISTS. In Aug 2026, disk-full (ENOSPC) builds left pioarduino's
custom_sdkconfig machinery half-applied: images linked against the wrong or a
partial kernel still PASSED `esptool image_info` and on-device flash verify, yet
boot-looped. That cost a month of misdiagnosis (the phantom "IROM cliff";
x4pro-program/CONTEXT.md §8 "Disk-full builds" and the 2026-09-24 entries).
Separately, every custom-core env of one chip shares ONE mutable package
(framework-arduinoespressif32-libs/<chip>), so building env B can leave env A
linking B's kernel unless pioarduino's rebuild logic notices. The only
trustworthy evidence is the ARTIFACT, so this guard:

  1. PRE-BUILD: refuses to build when free disk is low on the project
     filesystem or the one holding ~/.platformio (the ENOSPC guard).
  2. POST-BUILD: checks the per-env expectations in MANIFEST below against the
     build products -- firmware.map (primarily), firmware.bin vs. the env's
     app partition, and the env's generated sdkconfig.<env> where the map
     cannot show a setting. It never reads the shared framework package.

The guard is read-only: it never changes a build product.

Wiring: platformio.ini [build_integrity] extra_scripts lists this file as
both `pre:` (disk preflight) and `post:` (artifact check); fork envs only. Knobs: CROSSPOINT_MIN_FREE_GB (default 5) and, for emergencies only,
CROSSPOINT_BUILD_INTEGRITY=off.

Standalone (vet a stashed artifact; stdlib only):
  python scripts/build_integrity.py --env x4pro-combo --map firmware.map \
      [--bin firmware.bin] [--sdkconfig sdkconfig.x4pro-combo] \
      [--partitions partitions.csv] [-v]
  python scripts/build_integrity.py --list          # print the manifest
  python scripts/build_integrity.py --preflight     # disk check only
"""

import argparse
import os
import re
import shutil
import sys
import time
from pathlib import Path

DEFAULT_MIN_FREE_GB = 5.0
SLOT_WARN_FRACTION = 0.90
TAG = "[build-integrity]"

# --------------------------------------------------------------------------
# Expectations manifest.
#
# Kinds (all regexes are Python `re`, matched per line with re.MULTILINE):
#   forbid_text    pattern must not occur ANYWHERE in the map (any region)
#   require_symbol a symbol matching pattern is DEFINED at a nonzero address
#                  in the memory map (C++ names are demangled in the map)
#   forbid_symbol  no such symbol is defined
#   require_object an input section "<section> <archive(member)>" matching
#                  pattern is PLACED with nonzero size (not discarded)
#   forbid_object  no such input section is placed
#   sdkconfig      KEY must have VALUE in the env's generated sdkconfig.<env>
#                  ("n" also matches "# KEY is not set"); skipped with a
#                  warning when that file is absent
# --------------------------------------------------------------------------

REBUILD_FIX = (
    "rebuild this env once from a clean state (rm -rf .pio/build/{env} "
    "sdkconfig.{env}; pio run -e {env}); if it persists, apply the "
    "CONTEXT.md §8 disk-full recovery (delete BOTH "
    "~/.platformio/packages/framework-arduinoespressif32{{,-libs}} plus the "
    "project scaffold clean)"
)


def _x(kind, pattern, what, why, value=None):
    return {"kind": kind, "pattern": pattern, "what": what, "why": why,
            "value": value}


# Invariants every guarded env shares (all four build a custom kernel).
CUSTOM_KERNEL = [
    _x("forbid_text", r"libc_nano",
       "no newlib-nano in the link",
       "newlib-nano printf was reverted on r5 (74288261); nano in this link "
       "means the application link used another kernel's libc config (the "
       "shared framework package or a stale patch_arduino_rom_libc.py "
       "result) -- the shared framework package likely holds another env's "
       "kernel"),
    _x("require_object", r"/libc\.a\(libc_a-vfprintf\.o\)",
       "full-libc vfprintf placed (libc.a(libc_a-vfprintf.o))",
       "the full newlib printf is missing, so this is not the full-libc "
       "kernel this env is configured for"),
    _x("require_object", r"libespcoredump\.a\(core_dump_flash\.c\.o(?:bj)?\)",
       "coredump-to-flash code placed (core_dump_flash.c)",
       "no coredump-to-flash code: crashes would leave no dump in the "
       "coredump partition, and the kernel is not the configured one"),
    _x("forbid_object", r"libespressif__esp_diagnostics\.a\(",
       "no esp_diagnostics objects (custom_component_remove applied)",
       "esp_diagnostics is linked, which only the STOCK prebuilt Arduino "
       "kernel does -- custom_sdkconfig/custom_component_remove was NOT "
       "applied to this artifact (the Aug-2026 half-applied-kernel failure "
       "mode: the image boots but configured kernel features are absent)"),
]

COMBO_COMMON = CUSTOM_KERNEL + [
    _x("require_symbol", r"^esp_pm_impl_init$",
       "esp_pm implementation placed (esp_pm_impl_init; PM_ENABLE on)",
       "no esp_pm implementation: the artifact was linked against a kernel "
       "WITHOUT CONFIG_PM_ENABLE (stock, or another env's kernel from the "
       "shared framework package); light sleep/DFS would silently be off"),
    _x("require_symbol", r"^vApplicationSleep$",
       "tickless-idle hook placed (vApplicationSleep)",
       "no vApplicationSleep: the kernel lacks "
       "CONFIG_FREERTOS_USE_TICKLESS_IDLE, so the idle light-sleep win is "
       "gone"),
    _x("sdkconfig", "CONFIG_PM_ENABLE", "sdkconfig PM_ENABLE=y",
       "the generated sdkconfig disagrees with custom_sdkconfig", "y"),
    _x("sdkconfig", "CONFIG_FREERTOS_USE_TICKLESS_IDLE",
       "sdkconfig TICKLESS_IDLE=y",
       "the generated sdkconfig disagrees with custom_sdkconfig", "y"),
    _x("sdkconfig", "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH",
       "sdkconfig COREDUMP_ENABLE_TO_FLASH=y",
       "coredump-to-flash was restored on r5 (34856cb0)", "y"),
    _x("sdkconfig", "CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL",
       "sdkconfig ASSERTION_LEVEL=2 (full file/line/expr)",
       "silent assertions were reverted on r5 (d21b3e2c)", "2"),
    _x("sdkconfig", "CONFIG_LIBC_NEWLIB_NANO_FORMAT",
       "sdkconfig LIBC_NEWLIB_NANO_FORMAT off",
       "newlib-nano was reverted on r5 (74288261)", "n"),
]

PMSTATS_ONLY_SYMBOL = r"^esp_pm_light_sleep_register_cbs$"
PMSTATS_ONLY_OBJECT = r"^\.bss\.s_time_in_mode .*libesp_pm\.a\(pm_impl\.c\.o(?:bj)?\)"

MANIFEST = {
    "x4pro-combo": COMBO_COMMON + [
        # The pmstats kernel lives in the SAME esp32s3 package slot; these
        # catch it leaking into a plain combo build.
        _x("forbid_object", PMSTATS_ONLY_OBJECT,
           "no PM_PROFILING tables (pmstats kernel not leaked in)",
           "esp_pm profiling tables are linked: this is the "
           "x4pro-combo-pmstats kernel, left in the shared framework package "
           "by a pmstats build"),
        _x("forbid_symbol", PMSTATS_ONLY_SYMBOL,
           "no light-sleep callbacks (pmstats kernel not leaked in)",
           "esp_pm_light_sleep_register_cbs is linked: this is the "
           "x4pro-combo-pmstats kernel, left in the shared framework package "
           "by a pmstats build"),
        _x("sdkconfig", "CONFIG_PM_PROFILING", "sdkconfig PM_PROFILING off",
           "only the pmstats env profiles esp_pm", "n"),
    ],
    "x4pro-combo-pmstats": COMBO_COMMON + [
        _x("require_object", PMSTATS_ONLY_OBJECT,
           "PM_PROFILING tables placed (.bss.s_time_in_mode)",
           "no esp_pm profiling tables: the kernel lacks CONFIG_PM_PROFILING "
           "(likely the plain x4pro-combo kernel from the shared framework "
           "package), so CMD:PMSTATS would report nothing"),
        _x("require_symbol", PMSTATS_ONLY_SYMBOL,
           "light-sleep callbacks placed (esp_pm_light_sleep_register_cbs)",
           "the kernel lacks CONFIG_PM_LIGHT_SLEEP_CALLBACKS, so the LSSTATS "
           "residency counters cannot register"),
        _x("sdkconfig", "CONFIG_PM_PROFILING", "sdkconfig PM_PROFILING=y",
           "re-enabled on r5 (6a8ce759)", "y"),
    ],
    # C3 envs: base + firmware_tuned_c3 kernel, PM deliberately off. Their
    # generated sdkconfig is written under whichever C3 env last rebuilt the
    # core (usually sdkconfig.default), so it is not per-env: map checks only.
    "x4-rss": CUSTOM_KERNEL + [
        _x("forbid_symbol", r"^esp_pm_impl_init$",
           "no esp_pm (C3 kernel has PM off)",
           "esp_pm is linked, which no C3 env configures -- wrong kernel"),
        _x("require_symbol", r"^RssSyncActivity::",
           "RSS sync compiled in (RssSyncActivity)",
           "CROSSPOINT_RSS_SYNC code is missing: this is not an x4-rss "
           "build"),
        _x("forbid_symbol", r"^(softclock::|FlashcardStudyActivity::)",
           "no flashcards/soft clock (those are x4-flash)",
           "flashcard/soft-clock code is linked: this is an x4-flash "
           "artifact, not x4-rss"),
    ],
    "x4-flash": CUSTOM_KERNEL + [
        _x("forbid_symbol", r"^esp_pm_impl_init$",
           "no esp_pm (C3 kernel has PM off)",
           "esp_pm is linked, which no C3 env configures -- wrong kernel"),
        _x("require_symbol", r"^softclock::",
           "RTC-less soft clock compiled in (softclock::)",
           "CROSSPOINT_SOFT_CLOCK code is missing: the C3 has no RTC, so "
           "the clock would never be valid"),
        _x("require_symbol", r"^FlashcardStudyActivity::",
           "flashcards compiled in (FlashcardStudyActivity)",
           "CROSSPOINT_FLASHCARDS code is missing: this is not an x4-flash "
           "build"),
    ],
}


# --------------------------------------------------------------------------
# Disk preflight
# --------------------------------------------------------------------------

def min_free_gb(default=DEFAULT_MIN_FREE_GB):
    raw = os.environ.get("CROSSPOINT_MIN_FREE_GB")
    if raw:
        try:
            return float(raw)
        except ValueError:
            print(f"{TAG} WARNING: ignoring non-numeric "
                  f"CROSSPOINT_MIN_FREE_GB={raw!r}")
    return default


def disk_preflight(paths, threshold_gb):
    """Return (ok, summary_or_error_text). paths: {label: path}."""
    seen = {}
    for label, path in paths.items():
        p = Path(path).expanduser()
        while not p.exists() and p != p.parent:
            p = p.parent
        try:
            dev = p.stat().st_dev
        except OSError:
            continue
        seen.setdefault(dev, (label, p))
    results, bad = [], []
    for label, p in seen.values():
        free_gb = shutil.disk_usage(p).free / 1e9
        results.append(f"{label} {free_gb:.1f} GB free")
        if free_gb < threshold_gb:
            bad.append(f"  free disk {free_gb:.1f} GB < {threshold_gb:g} GB "
                       f"on the filesystem holding {label} ({p})")
    if bad:
        return False, "\n".join(
            [f"{TAG} DISK PREFLIGHT FAILED -- refusing to build."] + bad + [
                "  Aug-2026 ENOSPC builds produced silently-corrupt kernels "
                "(half-applied custom_sdkconfig; images passed esptool "
                "image_info and flash verify yet boot-looped).",
                "  Free space before building. If a build already ran out of "
                "space, the shared framework package may be damaged: apply "
                "the CONTEXT.md §8 disk-full recovery.",
                "  (Threshold: CROSSPOINT_MIN_FREE_GB, default "
                f"{DEFAULT_MIN_FREE_GB:g}.)"])
    return True, f"disk preflight OK ({', '.join(results)})"


# --------------------------------------------------------------------------
# Artifact parsing
# --------------------------------------------------------------------------

_IN_ONE = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (.+)$")
_IN_CONT = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (.+)$")
_SYM = re.compile(r"^\s+0x([0-9a-f]+)\s+([^\s(].*)$")
_SEC_ALONE = re.compile(r"^ (\S+)$")


class MapIndex:
    """What a GNU ld map says was actually placed in the image."""

    def __init__(self, path):
        self.path = Path(path)
        self.text = self.path.read_text(encoding="utf-8", errors="replace")
        start = self.text.find("\nLinker script and memory map")
        if start < 0:
            raise ValueError(
                f"{path} has no 'Linker script and memory map' section -- "
                "not a GNU ld map, or truncated (disk full while linking?)")
        end = self.text.find("\nCross Reference Table", start)
        region = self.text[start:end if end > 0 else None]
        objects, symbols = [], []
        pending = None
        for line in region.splitlines():
            m = _IN_ONE.match(line)
            if m:
                pending = None
                if int(m.group(2), 16) and int(m.group(3), 16):
                    objects.append(f"{m.group(1)} {m.group(4).strip()}")
                continue
            m = _IN_CONT.match(line)
            if m:
                if pending and int(m.group(1), 16) and int(m.group(2), 16):
                    objects.append(f"{pending} {m.group(3).strip()}")
                pending = None
                continue
            m = _SYM.match(line)
            if m:
                name = m.group(2).strip()
                if int(m.group(1), 16) and "=" not in name:
                    symbols.append(name)
                continue
            m = _SEC_ALONE.match(line)
            pending = m.group(1) if m else None
        self.objects = "\n".join(objects)
        self.symbols = "\n".join(symbols)


def read_sdkconfig(path):
    values = {}
    for line in Path(path).read_text(encoding="utf-8",
                                     errors="replace").splitlines():
        line = line.strip()
        m = re.match(r"^# (CONFIG_\w+) is not set$", line)
        if m:
            values[m.group(1)] = "n"
        elif line.startswith("CONFIG_") and "=" in line:
            key, _, value = line.partition("=")
            values[key] = value.strip().strip('"')
    return values


def app_slot_bytes(partitions_csv):
    """Smallest app partition size in the table (the image must fit every
    slot it can be OTA'd or flashed into)."""
    sizes = []
    for line in Path(partitions_csv).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[1] == "app" and cols[4]:
            s = cols[4].upper()
            mult = 1
            if s.endswith("K"):
                mult, s = 1024, s[:-1]
            elif s.endswith("M"):
                mult, s = 1024 * 1024, s[:-1]
            sizes.append(int(s, 0) * mult)
    return min(sizes) if sizes else None


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

def run_checks(env_name, map_path, bin_path=None, sdkconfig_path=None,
               partitions_path=None, verbose=False, elf_path=None,
               max_map_age_s=120):
    """Return (ok, report_text)."""
    if env_name not in MANIFEST:
        return False, (f"{TAG} {env_name}: no expectations in MANIFEST "
                       f"({Path(__file__).name}). Known envs: "
                       f"{', '.join(sorted(MANIFEST))}. Add an entry before "
                       "wiring the guard into a new env.")
    fix = REBUILD_FIX.format(env=env_name)
    fails, warns, passes = [], [], []

    map_path = Path(map_path)
    if not map_path.is_file():
        return False, (
            f"{TAG} {env_name}: FAILED -- linker map {map_path} is missing, "
            "so the kernel in this artifact cannot be verified. The map is "
            "a side product of the link and is NOT stored in the build cache "
            "(.cache), so an elf restored from that cache has none; the "
            "guard excludes guarded envs' elf from the cache, so force one "
            f"relink: rm .pio/build/{env_name}/firmware.elf; pio run -e "
            f"{env_name}")
    if elf_path and Path(elf_path).is_file():
        lag = Path(elf_path).stat().st_mtime - map_path.stat().st_mtime
        if lag > max_map_age_s:
            return False, (
                f"{TAG} {env_name}: FAILED -- {map_path} is {lag:.0f} s older "
                f"than {elf_path}; it describes an earlier link, not this "
                f"image. Force a relink: rm .pio/build/{env_name}/"
                f"firmware.elf; pio run -e {env_name}")
    try:
        idx = MapIndex(map_path)
    except ValueError as e:
        return False, f"{TAG} {env_name}: FAILED -- {e}. {fix}."

    sdk = None
    if sdkconfig_path and Path(sdkconfig_path).is_file():
        sdk = read_sdkconfig(sdkconfig_path)

    sdk_skipped = 0
    for ex in MANIFEST[env_name]:
        kind, pat, what = ex["kind"], ex["pattern"], ex["what"]
        if kind == "sdkconfig":
            if sdk is None:
                sdk_skipped += 1
                continue
            got = sdk.get(pat, "n" if ex["value"] == "n" else None)
            if got == ex["value"]:
                passes.append(what)
            else:
                fails.append(f"{what}: {Path(sdkconfig_path).name} has "
                             f"{pat}={got if got is not None else '(unset)'} "
                             f"-- {ex['why']}")
            continue
        rx = re.compile(pat, re.M)
        if kind == "forbid_text":
            n = len(rx.findall(idx.text))
            if n:
                fails.append(f"{what}: map contains {pat} ({n} refs) -- "
                             f"{ex['why']}")
            else:
                passes.append(what)
            continue
        blob = idx.symbols if kind.endswith("_symbol") else idx.objects
        hits = rx.findall(blob)
        if kind.startswith("require"):
            if hits:
                passes.append(what)
            else:
                fails.append(f"{what}: NOT FOUND in map -- {ex['why']}")
        else:
            if hits:
                first = rx.search(blob)
                line = blob[first.start():blob.find("\n", first.start())]
                fails.append(f"{what}: FOUND {len(hits)} match(es), e.g. "
                             f"'{line[:120]}' -- {ex['why']}")
            else:
                passes.append(what)

    if sdk_skipped:
        where = (f"{sdkconfig_path} not found" if sdkconfig_path
                 else "no --sdkconfig given")
        warns.append(f"{sdk_skipped} sdkconfig check(s) skipped ({where})")

    size_note = ""
    if bin_path and Path(bin_path).is_file():
        size = Path(bin_path).stat().st_size
        slot = (app_slot_bytes(partitions_path)
                if partitions_path and Path(partitions_path).is_file()
                else None)
        if slot is None:
            warns.append(f"partition check skipped: no app partition found "
                         f"({partitions_path})")
        else:
            pct = 100.0 * size / slot
            size_note = f", bin {size:,} B = {pct:.1f}% of app slot {slot:#x}"
            if size > slot:
                fails.append(
                    f"image fits app partition: {size:,} B > slot {slot:,} B "
                    f"({slot:#x}, {Path(partitions_path).name}) -- this "
                    "image cannot be flashed/OTA'd; shrink it or change the "
                    "partition table")
            else:
                passes.append("image fits app partition")
                if size > SLOT_WARN_FRACTION * slot:
                    warns.append(f"image uses {pct:.1f}% of its app slot "
                                 f"(> {SLOT_WARN_FRACTION:.0%})")
    elif bin_path:
        fails.append(f"firmware image {bin_path} missing")

    total = len(passes) + len(fails)
    out = []
    if fails:
        out.append(f"{TAG} {env_name}: FAILED {len(fails)} of {total} checks "
                   f"(map {map_path})")
        out += [f"  FAIL {f}" for f in fails]
        out += [f"  WARN {w}" for w in warns]
        if verbose:
            out += [f"  ok   {p}" for p in passes]
        out.append(f"  -> Do NOT flash this artifact. Unless a FAIL above says "
                   f"otherwise: {fix}.")
    else:
        out.append(f"{TAG} {env_name}: OK ({total} checks{size_note})")
        out += [f"  WARN {w}" for w in warns]
        if verbose:
            out += [f"  ok   {p}" for p in passes]
    return not fails, "\n".join(out)


# --------------------------------------------------------------------------
# PlatformIO hook
# --------------------------------------------------------------------------

def _pio_hook(env):
    """Listed twice in extra_scripts: the `pre:` load runs the disk preflight
    before anything (incl. pioarduino's core rebuild) touches the disk; the
    `post:` load registers the artifact check, once the platform has set
    PROGNAME and defined the firmware.bin target."""
    env_name = env.subst("$PIOENV")
    if os.environ.get("CROSSPOINT_BUILD_INTEGRITY", "").lower() in (
            "0", "off", "no", "false"):
        print(f"{TAG} WARNING: guard DISABLED via CROSSPOINT_BUILD_INTEGRITY "
              f"for {env_name} -- this build is unverified; do not flash it "
              "without running the standalone checker.")
        return
    if env_name not in MANIFEST:
        print(f"{TAG} {env_name}: no MANIFEST entry -- add one in "
              "scripts/build_integrity.py before wiring this env.")
        env.Exit(1)
    if env.get("CROSSPOINT_INTEGRITY_PREFLIGHT_DONE"):
        _pio_register_check(env, env_name)
        return
    env["CROSSPOINT_INTEGRITY_PREFLIGHT_DONE"] = True

    try:
        default_gb = float(env.GetProjectOption(
            "custom_build_integrity_min_free_gb", DEFAULT_MIN_FREE_GB))
    except (TypeError, ValueError):
        default_gb = DEFAULT_MIN_FREE_GB
    ok, text = disk_preflight({
        "the project": env.subst("$PROJECT_DIR"),
        "the build dir": env.subst("$PROJECT_BUILD_DIR"),
        "~/.platformio": env.subst("$PROJECT_CORE_DIR"),
        "the PlatformIO packages": env.subst("$PROJECT_PACKAGES_DIR"),
    }, min_free_gb(default_gb))
    print(text if not ok else f"{TAG} {env_name}: {text}")
    if not ok:
        env.Exit(1)


def _pio_register_check(env, env_name):
    from SCons.Script import AlwaysBuild, Default  # pylint: disable=import-error

    project_dir = Path(env.subst("$PROJECT_DIR"))

    def verify(target, source, env):  # pylint: disable=unused-argument
        build_dir = Path(env.subst("$BUILD_DIR"))
        progname = env.subst("$PROGNAME")
        partitions = env.subst("$PARTITIONS_TABLE_CSV") or str(
            project_dir / env.GetProjectOption("board_build.partitions",
                                               "partitions.csv"))
        t0 = time.monotonic()
        ok, report = run_checks(
            env_name,
            build_dir / f"{progname}.map",
            bin_path=build_dir / f"{progname}.bin",
            sdkconfig_path=project_dir / f"sdkconfig.{env_name}",
            partitions_path=partitions,
            elf_path=build_dir / f"{progname}.elf",
        )
        print(f"{report} [{time.monotonic() - t0:.1f} s]")
        return 0 if ok else 1

    # The linker map is a side product the SCons build cache (.cache) does
    # not store: an elf retrieved from the cache arrives WITHOUT a map. Always
    # link this env's elf for real so its map is the artifact's own. (Every
    # object file still comes from the cache; only the link step is forced.)
    env.NoCache(env.get("PIOMAINPROG") or "$BUILD_DIR/${PROGNAME}.elf")

    # An always-run default target downstream of firmware.bin: it runs after
    # every build, including no-op rebuilds, and a failure fails `pio run`.
    guard = env.Alias("build_integrity", "$BUILD_DIR/${PROGNAME}.bin",
                      env.Action(verify, "Verifying build integrity"))
    AlwaysBuild(guard)
    Default(guard)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Verify a firmware artifact against its env's kernel "
                    "expectations (see module docstring).")
    ap.add_argument("--env", help="env name (key in MANIFEST)")
    ap.add_argument("--map", help="linker map (firmware.map)")
    ap.add_argument("--bin", help="app image (firmware.bin) for the "
                                  "partition-fit check")
    ap.add_argument("--sdkconfig", help="generated sdkconfig.<env> "
                                        "(sdkconfig checks skip without it)")
    ap.add_argument("--partitions", default=str(
        Path(__file__).resolve().parent.parent / "partitions.csv"),
        help="partition table CSV (default: repo partitions.csv)")
    ap.add_argument("--preflight", action="store_true",
                    help="run only the disk preflight (cwd + ~/.platformio)")
    ap.add_argument("--list", action="store_true",
                    help="print the expectations manifest")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also list passing checks")
    a = ap.parse_args()
    if a.list:
        for name, exps in MANIFEST.items():
            print(f"{name}:")
            for ex in exps:
                print(f"  {ex['kind']:<15} {ex['what']}")
        sys.exit(0)
    if a.preflight:
        ok, text = disk_preflight({
            "the current directory": os.getcwd(),
            "~/.platformio": os.environ.get("PLATFORMIO_CORE_DIR",
                                            "~/.platformio")}, min_free_gb())
        print(text if not ok else f"{TAG} {text}")
        sys.exit(0 if ok else 1)
    if not a.env or not a.map:
        ap.error("--env and --map are required (or --preflight / --list)")
    t0 = time.monotonic()
    ok, report = run_checks(a.env, a.map, a.bin, a.sdkconfig, a.partitions,
                            verbose=a.verbose)
    print(f"{report} [{time.monotonic() - t0:.1f} s]")
    sys.exit(0 if ok else 1)
elif "Import" in globals():
    Import("env")  # noqa: F821 pylint: disable=undefined-variable
    _pio_hook(env)  # noqa: F821 pylint: disable=undefined-variable
