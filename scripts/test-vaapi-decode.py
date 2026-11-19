#!/usr/bin/env python3
"""Compare every VA-API output frame with software decoding on the device."""
import argparse
import os
from pathlib import Path
import subprocess


def decode(path, pixel_format, hardware, device, env):
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror"]
    if hardware:
        command += ["-hwaccel", "vaapi", "-hwaccel_device", device,
                    "-hwaccel_output_format", "vaapi"]
    command += ["-i", str(path), "-map", "0:v:0", "-an", "-sn",
                "-vf", ("hwdownload," if hardware else "") + "format=" + pixel_format,
                "-fps_mode", "passthrough", "-f", "framemd5", "-"]
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return [line.rsplit(",", 1)[-1].strip() for line in result.stdout.splitlines()
            if line and not line.startswith("#")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/dri/renderD128")
    parser.add_argument("--driver-path", type=Path)
    parser.add_argument("clips", nargs="+", type=Path)
    args = parser.parse_args()
    env = {**os.environ, "LIBVA_DRIVER_NAME": "msm"}
    if args.driver_path:
        env["LIBVA_DRIVERS_PATH"] = str(args.driver_path.resolve())
    for path in args.clips:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
             "stream=pix_fmt", "-of", "default=nw=1:nk=1", str(path)],
            capture_output=True, text=True, check=True, timeout=30)
        pixel_format = "p010le" if probe.stdout.strip() == "yuv420p10le" else "nv12"
        software = decode(path, pixel_format, False, args.device, env)
        hardware = decode(path, pixel_format, True, args.device, env)
        if not software or software != hardware:
            raise RuntimeError(
                f"{path}: hardware/software mismatch ({len(hardware)}/{len(software)} frames)")
        print(f"{path.name}: {len(hardware)} {pixel_format} frames, bit-exact hardware decode",
              flush=True)


if __name__ == "__main__":
    main()
