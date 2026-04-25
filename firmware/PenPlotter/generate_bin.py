"""
Post-build script: converts firmware.hex → firmware.bin for SD card flashing.
PlatformIO runs this automatically after a successful build.

The Creality Melzi bootloader reads firmware.bin from the SD card root on boot.
Copy the generated firmware.bin to the root of a FAT32-formatted micro SD card.
"""
import os
import subprocess
Import("env")

def make_bin(source, target, env):
    hex_path = str(target[0])
    bin_path = os.path.join(env.subst("$PROJECT_DIR"), "firmware.bin")

    # avr-objcopy ships with avr-gcc (included in PlatformIO toolchain)
    avr_objcopy = os.path.join(
        env.subst("$PYTHONPATH"),   # fallback
        "avr-objcopy"
    )

    # Use the toolchain binary from PlatformIO's package cache
    toolchain = env.subst("$PIOHOME_DIR") + "/packages/toolchain-atmelavr/bin/avr-objcopy"
    if not os.path.isfile(toolchain):
        toolchain = "avr-objcopy"   # assume it's on PATH

    try:
        subprocess.check_call([
            toolchain,
            "-I", "ihex",
            "-O", "binary",
            hex_path,
            bin_path
        ])
        print(f"\n✅  firmware.bin created → {bin_path}")
        print("    Copy firmware.bin to the root of a FAT32 micro SD card.")
        print("    Insert card and power on — bootloader will flash automatically.\n")
    except Exception as e:
        print(f"\n⚠️  Could not generate firmware.bin: {e}")
        print(f"    Run manually:  avr-objcopy -I ihex -O binary {hex_path} firmware.bin\n")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", make_bin)
