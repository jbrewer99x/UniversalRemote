Import("env")

import os
import shutil

def rename_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")

    source_file = os.path.join(build_dir, "firmware.bin")
    target_file = os.path.join(build_dir, "universal-remote.bin")

    if os.path.exists(source_file):
        shutil.copy2(source_file, target_file)
        print(f"Created: {target_file}")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", rename_firmware)