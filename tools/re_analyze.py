import json
import os
import struct
import sys


def inspect_elf(path):
    with open(path, "rb") as f:
        data = f.read()

    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize = struct.unpack_from("<H", data, 58)[0]
    e_shnum = struct.unpack_from("<H", data, 60)[0]
    e_shstrndx = struct.unpack_from("<H", data, 62)[0]
    shstr_off = struct.unpack_from("<Q", data[e_shoff + e_shstrndx * e_shentsize :], 24)[0]

    sections = []
    for i in range(e_shnum):
        sh = data[e_shoff + i * e_shentsize : e_shoff + (i + 1) * e_shentsize]
        (
            sh_name,
            sh_type,
            sh_flags,
            sh_addr,
            sh_offset,
            sh_size,
            sh_link,
            sh_info,
            sh_addralign,
            sh_entsize,
        ) = struct.unpack("<IIQQQQIIQQ", sh)
        name = data[shstr_off + sh_name : data.find(b"\x00", shstr_off + sh_name)].decode(
            "ascii", errors="ignore"
        )
        sections.append((name, sh_type, sh_addr, sh_offset, sh_size, sh_link, sh_entsize))

    dynsym = next(s for s in sections if s[0] == ".dynsym")
    dynstr = sections[dynsym[5]]
    sym_data = data[dynsym[3] : dynsym[3] + dynsym[4]]
    str_data = data[dynstr[3] : dynstr[3] + dynstr[4]]

    exports = []
    imports = []
    for i in range(0, len(sym_data), dynsym[6]):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack(
            "<IBBHQQ", sym_data[i : i + dynsym[6]]
        )
        name = str_data[st_name : str_data.find(b"\x00", st_name)].decode(
            "ascii", errors="ignore"
        )
        if not name:
            continue
        if st_shndx == 0:
            imports.append(name)
        else:
            exports.append((name, st_value))

    return {
        "file": os.path.basename(path),
        "size": len(data),
        "exports_count": len(exports),
        "imports_count": len(imports),
        "exports": exports,
        "imports": imports,
    }


def main():
    if len(sys.argv) < 2:
        print("Usage: python tools/re_analyze.py <dir_with_pulled_vendor_blobs>")
        sys.exit(1)

    blob_dir = sys.argv[1]
    for fn in ["vulkan.powervr.so", "libsrv_um.so", "libusc.so"]:
        p = os.path.join(blob_dir, fn)
        if not os.path.exists(p):
            print(f"Missing: {p}")
            continue
        info = inspect_elf(p)
        print(f"=== {info['file']} ({info['size']} bytes) ===")
        print(f"  Exports: {info['exports_count']}, Imports: {info['imports_count']}")
        vk_or_hal = [e[0] for e in info["exports"] if "vk" in e[0].lower() or e[0] == "HMI"]
        if vk_or_hal:
            print(f"  VK/HAL exports: {vk_or_hal}")

    vkjson_path = os.path.join(blob_dir, "vkjson.json")
    if os.path.exists(vkjson_path):
        for enc in ("utf-8", "utf-16"):
            try:
                with open(vkjson_path, "r", encoding=enc) as f:
                    d = json.load(f)
                break
            except Exception:
                continue
        dev = d["devices"][0]
        print("=== vkjson ===")
        print("  Device:", dev["properties"]["deviceName"])
        print("  Driver Version:", hex(int(dev["properties"]["driverVersion"])))
        print("  API Version:", hex(int(dev["properties"]["apiVersion"])))
        print("  Extensions:", len(dev["extensions"]))
        print("  Memory Heaps:", dev["memory"]["memoryHeaps"])
        print("  Memory Types:", dev["memory"]["memoryTypes"])


if __name__ == "__main__":
    main()
