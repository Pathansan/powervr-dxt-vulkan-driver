import argparse
import io
import json
import os
import tarfile
import zstandard


def main():
    parser = argparse.ArgumentParser(description="Package PowerVR DXT Compat Vulkan ICD into a GameNative .wcp driver")
    parser.add_argument("--so", required=True, help="Path to compiled libvulkan.powervr_compat.so")
    parser.add_argument("--out", required=True, help="Output .wcp path")
    parser.add_argument("--version", default="0.1.0-pvr-dxt", help="Driver versionName")
    args = parser.parse_args()

    with open(args.so, "rb") as f:
        so_bytes = f.read()

    profile = {
        "type": "ADRENO",
        "versionName": args.version,
        "versionCode": 10001,
        "description": "PowerVR DXT-48-1536 MC1 Translation Vulkan ICD (ARM64EC x18 guard + VK_EXT_map_memory_placed)",
        "files": [
            {
                "source": "libvulkan.powervr_compat.so",
                "target": "${libdir}/libvulkan.powervr_compat.so",
            }
        ],
    }
    profile_bytes = json.dumps(profile, indent=2).encode("utf-8")

    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w") as tf:
        ti = tarfile.TarInfo(name="profile.json")
        ti.size = len(profile_bytes)
        ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(profile_bytes))

        ti = tarfile.TarInfo(name="libvulkan.powervr_compat.so")
        ti.size = len(so_bytes)
        ti.mode = 0o755
        tf.addfile(ti, io.BytesIO(so_bytes))

    cctx = zstandard.ZstdCompressor(level=19)
    compressed = cctx.compress(tar_buf.getvalue())

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(compressed)

    print(f"Packaged {args.out} ({len(compressed)} bytes)")


if __name__ == "__main__":
    main()
