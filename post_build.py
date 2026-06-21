# post_build.py
#
# PlatformIO post-build hook. After the normal application image
# (firmware.bin) is produced, also emit a single *merged* image, ft8lora.bin,
# that contains the bootloader + partition table + boot_app0 + application
# concatenated at their correct flash offsets. That single file can be flashed
# in one shot at offset 0x0 -- the friendly artifact to publish for end users
# (works with esptool and with browser flashers such as ESP Web Tools).
#
# Both files land in the build directory, e.g.
#   .pio/build/heltec_wifi_lora_32_v3/firmware.bin   (app only)
#   .pio/build/heltec_wifi_lora_32_v3/ft8lora.bin    (full, flash @ 0x0)
#
# All offsets/paths are taken from PlatformIO's own build state
# (FLASH_EXTRA_IMAGES + ESP32_APP_OFFSET), so they are always correct for this
# board and MCU -- nothing is hard-coded.

import os
from os.path import join, isfile

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def _flash_freq(board):
    # build.f_flash looks like "80000000L" -> "80m"
    raw = str(board.get("build.f_flash", "40000000L")).rstrip("L")
    try:
        return "%dm" % (int(raw) // 1000000)
    except ValueError:
        return "40m"


def _esptool_invocation(env):
    # Prefer the esptool.py that this platform ships; fall back to running it as
    # a module from the same Python, which is more portable across forks.
    py = env.subst("$PYTHONEXE")
    pkg = env.PioPlatform().get_package_dir("tool-esptoolpy")
    if pkg:
        cand = join(pkg, "esptool.py")
        if isfile(cand):
            return '"%s" "%s"' % (py, cand)
    return '"%s" -m esptool' % py


def merge_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    app_bin = join(build_dir, "firmware.bin")
    merged_bin = join(build_dir, "ft8lora.bin")

    board = env.BoardConfig()
    chip = board.get("build.mcu", "esp32")
    flash_mode = env.subst("$BOARD_FLASH_MODE") or board.get("build.flash_mode", "dio")
    flash_size = board.get("upload.flash_size", "8MB")  # Heltec V3 has 8MB
    flash_freq = _flash_freq(board)

    # [offset, image] for bootloader / partitions / boot_app0 (paths may contain
    # build variables, so substitute each one).
    parts = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        parts += [str(offset), env.subst(image)]
    # the application image itself, at its app offset
    app_offset = env.subst("$ESP32_APP_OFFSET") or "0x10000"
    parts += [app_offset, app_bin]

    # quote anything that looks like a path
    def q(x):
        return '"%s"' % x if (os.sep in x or "/" in x) else x

    cmd = " ".join([
        _esptool_invocation(env),
        "--chip", chip,
        "merge_bin",
        "-o", '"%s"' % merged_bin,
        "--flash_mode", flash_mode,
        "--flash_freq", flash_freq,
        "--flash_size", flash_size,
    ] + [q(p) for p in parts])

    print("")
    print("Building merged flash image -> %s" % merged_bin)
    rc = env.Execute(cmd)
    if rc:
        print("!! merge_bin failed (exit %s). The app firmware.bin is still valid." % rc)
        return
    print("Merged firmware ready: %s" % merged_bin)
    print("Flash a blank board in one shot with:")
    print('  esptool.py --chip %s write_flash 0x0 "%s"' % (chip, merged_bin))
    print("")


# Run the merge right after the application .bin is created.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_firmware)  # noqa: F821
