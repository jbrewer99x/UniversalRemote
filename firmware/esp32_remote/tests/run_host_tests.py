"""Run policy and real LVGL tests without ESP32 hardware. Requires gcc/g++.

PlatformIO must have resolved the project's pinned LVGL dependency first.
Build objects and optional screen captures live in the supplied temporary directory.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import os
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def png_from_ppm(path):
    data = path.read_bytes().split(b"\n", 3)[3]
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    scanlines = b"".join(b"\0" + data[y * 720:(y + 1) * 720] for y in range(320))
    path.with_suffix(".png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 240, 320, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    build = args.build_dir or Path(tempfile.mkdtemp(prefix="remote-ui-tests-"))
    build.mkdir(parents=True, exist_ok=True)
    includes = ["-I", ROOT / "include"]
    run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *includes,
         ROOT / "tests/test_policies.cpp", "-o", build / "policies"])
    run([build / "policies"])
    host_flags = ["-std=c++17", "-DCONFIG_BT_ENABLED", *includes, "-I", ROOT / "tests/host"]
    for name, sources in [("power", ["power_manager.cpp"]),
                          ("audio_power", ["power_manager.cpp", "audio_player.cpp"])]:
        run(["g++", *host_flags, ROOT / f"tests/test_{name}.cpp",
             *(ROOT / "src" / source for source in sources), "-o", build / name])
        run([build / name], timeout=10)
    run([build / "power", "clock-failure"], timeout=10)
    lvgl = ROOT / ".pio/libdeps/waveshare_esp32_s3_touch_lcd_2_8/lvgl"
    if not lvgl.is_dir():
        raise SystemExit("Resolve the PlatformIO LVGL dependency before running display tests.")
    common = ["-O1", "-DLV_CONF_INCLUDE_SIMPLE", *includes, "-I", lvgl]
    # A content signature prevents reusing objects after config/source changes.
    import hashlib
    config = (ROOT / "include/lv_conf.h").read_bytes()
    def compile_c(source):
        digest = hashlib.sha256(config + source.read_bytes()).hexdigest()[:16]
        obj = build / (source.relative_to(lvgl).as_posix().replace("/", "_") + digest + ".o")
        if not obj.exists():
            run(["gcc", "-std=c99", *common, "-c", source, "-o", obj], stdout=subprocess.DEVNULL)
        return obj
    with ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as pool:
        objects = list(pool.map(compile_c, sorted((lvgl / "src").rglob("*.c"))))
    run(["g++", *host_flags, *common,
         ROOT / "tests/test_display.cpp", ROOT / "src/display.cpp", ROOT / "src/power_manager.cpp", *objects,
         "-lm", "-o", build / "display"])
    run([build / "display", "allocation-failure"], cwd=build, timeout=10)
    run([build / "display"], cwd=build, timeout=30)
    for capture in build.glob("*.ppm"):
        png_from_ppm(capture)
    print(f"Host tests and rendered screen captures: {build}")


if __name__ == "__main__":
    main()
