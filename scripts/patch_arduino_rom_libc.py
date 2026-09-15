"""Work around pioarduino's custom-SDK libc linker configuration gap.

In pioarduino 55.03.311, custom_sdkconfig rebuilds SDK libraries and configuration
headers, but the Arduino application still uses the packaged libc LINKFLAGS and
LIBS from <mcu>/pioarduino-build.py, which are generated once from the PREBUILT
kernel and never re-derived. Any sdkconfig option whose ESP-IDF implementation is
partly a link-line decision therefore lands half-applied: the rebuilt SDK
libraries honour it and the application link does not. Both hooks below close one
such gap. Remove them once pioarduino derives the application's libc linker flags
from the rebuilt SDK configuration.
"""
from pathlib import Path

# Old and new Kconfig spellings of the same option; components/newlib/
# sdkconfig.rename:12 maps the first onto the second.
NANO_FORMAT_KEYS = ("CONFIG_LIBC_NEWLIB_NANO_FORMAT", "CONFIG_NEWLIB_NANO_FORMAT")


def _requested_sdkconfig(env):
    """The env's own custom_sdkconfig as a dict. Per-env by construction."""
    requested = {}
    for line in env.GetProjectOption("custom_sdkconfig", "").splitlines():
        key, separator, value = line.strip().partition("=")
        if separator:
            requested[key] = value.strip()
    return requested


def configure_newlib_nano(env):
    """Point the application link at libc_nano.a when the env asks for nano.

    ESP-IDF implements CONFIG_LIBC_NEWLIB_NANO_FORMAT in two halves. The kernel
    half is a Kconfig value the rebuilt SDK libraries pick up on their own. The
    application half is components/newlib/project_include.cmake adding
    "--specs=<nano.specs>" to LINK_OPTIONS, which rewrites the driver's -lc into
    -lc_nano. pioarduino runs no CMake for the application, so without this hook
    the link keeps the full libc: it fails outright on the ROM stub table's
    references to _printf_float/_scanf_float (libnewlib.a(newlib_init.c.o)),
    which only libc_nano.a defines, and even if it did not, none of the ~42 KB
    the option exists to save would come out.

    Swapping the library in place rather than passing the specs file is
    deliberate: pioarduino names -lc explicitly in LIBS, so a specs-supplied
    -lc_nano would land AFTER it and the full libc would win every symbol.
    """
    # The outer core-generation pass has not installed its new sdkconfig yet.
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    requested = _requested_sdkconfig(env)
    if not any(requested.get(key) == "y" for key in NANO_FORMAT_KEYS):
        return

    mcu = env.BoardConfig().get("build.mcu")
    libs_dir = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")) / mcu
    sdkconfig_path = libs_dir / "sdkconfig"
    if not sdkconfig_path.is_file():
        raise RuntimeError(f"nano retarget: {sdkconfig_path} missing — framework package layout changed?")
    config = sdkconfig_path.read_text(encoding="utf-8").splitlines()
    # Accept either Kconfig spelling: IDF renamed NEWLIB_* to LIBC_* and maps
    # the old name via sdkconfig.rename, so generated files may carry either.
    if not any(f"{key}=y" in config for key in NANO_FORMAT_KEYS):
        raise RuntimeError(f"{mcu} framework sdkconfig does not carry the requested newlib-nano configuration")

    libs = list(env["LIBS"])
    swapped = 0
    for index, entry in enumerate(libs):
        if str(entry) == "-lc":
            libs[index] = "-lc_nano"
            swapped += 1
        elif str(entry) == "c":
            libs[index] = "c_nano"
            swapped += 1
    if swapped != 1:
        raise RuntimeError(
            f"Expected exactly one libc entry in LIBS to retarget at nano, found {swapped}. "
            "If this fires after a pioarduino upgrade, check whether the core-generation "
            "pass sentinel (ARDUINO_LIB_COMPILE_FLAG == 'Build') was renamed."
        )
    env.Replace(LIBS=libs)


def configure_rom_libc(env):
    if env.BoardConfig().get("build.mcu") != "esp32c3":
        return
    # The outer core-generation pass has not installed its new sdkconfig yet.
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    requested = env.GetProjectOption("custom_sdkconfig", "")
    libc_setting = None
    for line in requested.splitlines():
        key, separator, value = line.strip().partition("=")
        if separator and key == "CONFIG_LIBC_OPTIMIZED_MISALIGNED_ACCESS":
            libc_setting = value.strip()
    if libc_setting != "n":
        return

    libs = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")) / "esp32c3"
    config = (libs / "sdkconfig").read_text(encoding="utf-8").splitlines()
    if "# CONFIG_LIBC_OPTIMIZED_MISALIGNED_ACCESS is not set" not in config:
        raise RuntimeError("C3 framework sdkconfig does not match the requested ROM libc configuration")
    rom = libs / "ld" / "esp32c3.rom.libc-suboptimal_for_misaligned_mem.ld"
    if not rom.is_file():
        raise RuntimeError(f"Missing C3 ROM libc linker script: {rom}")

    # pioarduino keeps the prebuilt package's -u anchors after a custom SDK rebuild.
    # Mirror ESP-IDF's esp_rom selection so flash-off callers still reach ROM.
    functions = ("memcpy", "memmove", "memcmp", "strcpy", "strncpy", "strncmp", "strcmp")
    anchors = {f"esp_libc_include_{name}_impl" for name in functions}
    flags = list(env["LINKFLAGS"])
    filtered = []
    index = 0
    while index < len(flags):
        if flags[index] == "-u" and index + 1 < len(flags) and flags[index + 1] in anchors:
            index += 2
        else:
            filtered.append(flags[index])
            index += 1
    if str(rom) not in filtered:
        filtered.extend(["-T", str(rom)])
    env.Replace(LINKFLAGS=filtered)


Import("env")  # noqa: F821 -- provided by PlatformIO
configure_rom_libc(env)  # noqa: F821
configure_newlib_nano(env)  # noqa: F821
