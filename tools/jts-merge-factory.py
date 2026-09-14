# Builds firmware.factory.bin (bootloader + partition table + boot_app0 + app, one image flashable
# at offset 0x0) after every build. pioarduino's platform-espressif32 did this automatically;
# the official espressif32 package (house_esp32 moved here 2026-09-12, see PATCHES.md) does not,
# and both the panel's "USB flash image" button and jts-firmware.yml expect this file to exist
# at .pio/build/<env>/firmware.factory.bin.
Import("env")
import os

FLASH_MODE = "dio"
FLASH_FREQ = "40m"
FLASH_SIZE = "4MB"


def merge_factory_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware = os.path.join(build_dir, "firmware.bin")
    if not all(os.path.exists(p) for p in (bootloader, partitions, firmware)):
        print("jts-merge-factory: missing bootloader/partitions/firmware.bin, skipping")
        return

    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")
    esptool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    esptool = os.path.join(esptool_dir, "esptool.py")
    out = os.path.join(build_dir, "firmware.factory.bin")

    env.Execute(" ".join([
        '"$PYTHONEXE"', f'"{esptool}"', "--chip", "esp32", "merge_bin",
        "-o", f'"{out}"',
        "--flash_mode", FLASH_MODE, "--flash_freq", FLASH_FREQ, "--flash_size", FLASH_SIZE,
        "0x1000", f'"{bootloader}"',
        "0x8000", f'"{partitions}"',
        "0xe000", f'"{boot_app0}"',
        "0x10000", f'"{firmware}"',
    ]))
    print(f"jts-merge-factory: wrote {out}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_factory_bin)
