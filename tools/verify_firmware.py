#!/usr/bin/env python3
"""Verify the merged ESP32-C3 firmware layout produced by idf.py merge-bin."""

from __future__ import annotations

import sys
import struct
from pathlib import Path


EXPECTED_IMAGES = (
    (0x0000, "bootloader/bootloader.bin"),
    (0x8000, "partition_table/partition-table.bin"),
    (0x10000, "FoloToy-AI-Passport.bin"),
)


def verify_identity_window(table: bytes) -> None:
    """Reject any partition that could erase the board identity on mount."""
    for at in range(0, len(table) - 31, 32):
        magic, _, _, offset, size, label, _ = struct.unpack_from("<HBBII16sI", table, at)
        if magic != 0x50AA:
            break
        if offset < 0x35A000 and offset + size > 0x356000:
            name = label.split(b"\0", 1)[0].decode("ascii", errors="replace")
            raise ValueError(f"partition {name} overlaps protected identity window")


def app_contains_ram(image: bytes, address: int, expected: bytes) -> bool:
    """Check actual initialized bytes, not just an ELF's virtual section size."""
    if len(image) < 24 or image[0] != 0xE9:
        return False
    cursor = 24
    for _ in range(image[1]):
        if cursor + 8 > len(image):
            return False
        start, size = struct.unpack_from("<II", image, cursor)
        cursor += 8
        if cursor + size > len(image):
            return False
        if start <= address and address + len(expected) <= start + size:
            offset = cursor + address - start
            return image[offset:offset + len(expected)] == expected
        cursor += size
    return False


def verify_sqlite_ram(build_dir: Path) -> None:
    # pyelftools is supplied by the activated ESP-IDF environment. Import here
    # so the pure partition/segment unit tests remain dependency-free.
    from elftools.elf.elffile import ELFFile

    with (build_dir / "FoloToy-AI-Passport.elf").open("rb") as stream:
        elf = ELFFile(stream)
        state = elf.get_section_by_name(".feishu_sqlite.data")
        if state is None:
            return  # Existing firmware predating the SQL component.
        symbols = elf.get_section_by_name(".symtab")

        def symbol(name: str):
            found = symbols.get_symbol_by_name(name)
            if not found or len(found) != 1:
                raise ValueError(f"missing/ambiguous SQLite layout symbol: {name}")
            return found[0]

        start, end = state["sh_addr"], state["sh_addr"] + state["sh_size"]
        if start != symbol("_feishu_sqlite_data_start")["st_value"] or end != symbol("_feishu_sqlite_data_end")["st_value"]:
            raise ValueError("SQLite initialized section boundaries mismatch")
        reservation = symbol("reserved_region_feishu_sqlite_state")
        table_start = symbol("soc_reserved_memory_region_start")["st_value"]
        table_end = symbol("soc_reserved_memory_region_end")["st_value"]
        at = reservation["st_value"]
        if not table_start <= at <= table_end - 8:
            raise ValueError("SQLite region is not registered in the SDK heap exclusion table")
        table = elf.get_section(reservation["st_shndx"])
        bounds = struct.unpack_from("<II", table.data(), at - table["sh_addr"])
        if bounds != (start, end):
            raise ValueError("SQLite region could be allocated as heap memory")
        image = (build_dir / "FoloToy-AI-Passport.bin").read_bytes()
        if not app_contains_ram(image, start, state.data()):
            raise ValueError("SQLite initialized state is missing or different in the application BIN")
        print(f"Verified SQLite initialized RAM: {end-start} bytes, loaded and excluded from heap")


def verify_lvgl_pool(build_dir: Path) -> None:
    """Verify either the historical bounded pool or the explicit shared allocator."""
    from elftools.elf.elffile import ELFFile
    config = (build_dir / "sdkconfig").read_text(encoding="utf-8")
    with (build_dir / "FoloToy-AI-Passport.elf").open("rb") as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name(".symtab")
        pools = [s for s in symbols.iter_symbols() if s.name.startswith("work_mem_int.")]
        if "CONFIG_LV_USE_CLIB_MALLOC=y" in config:
            if pools or "CONFIG_LV_USE_BUILTIN_MALLOC=y" in config:
                raise ValueError("Shared LVGL allocator unexpectedly reserves a static pool")
            link_map = (build_dir / "FoloToy-AI-Passport.map").read_text(encoding="utf-8")
            if "liblvgl__lvgl.a(lv_mem_core_clib.c.obj)" not in link_map:
                raise ValueError("LVGL shared allocator implementation is not linked")
            print("Verified LVGL shared C allocator: no static GUI pool")
        else:
            if "CONFIG_LV_USE_BUILTIN_MALLOC=y" not in config or len(pools) != 1 or pools[0]["st_size"] != 24 * 1024:
                raise ValueError(f"Unsupported LVGL allocator/pool: {[s['st_size'] for s in pools]}")
            print("Verified historical LVGL static pool: 24576 bytes")


def main() -> int:
    build_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    merged_path = build_dir / "FoloToy-AI-Passport-full.bin"
    flash_args_path = build_dir / "flash_args"

    if not merged_path.is_file() or not flash_args_path.is_file():
        print("ERROR: merged firmware or flash_args is missing", file=sys.stderr)
        return 1

    flash_args = flash_args_path.read_text(encoding="utf-8")
    if "--flash_size 8MB" not in flash_args:
        print("ERROR: flash_args does not select the required 8 MB flash size", file=sys.stderr)
        return 1

    merged = merged_path.read_bytes()
    try:
        verify_identity_window((build_dir / "partition_table/partition-table.bin").read_bytes())
        verify_sqlite_ram(build_dir)
        verify_lvgl_pool(build_dir)
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    for offset, relative_name in (*EXPECTED_IMAGES,(0x500000,"feishu-font.bin")):
        image_path = build_dir / relative_name
        if not image_path.is_file():
            print(f"ERROR: missing image {image_path}", file=sys.stderr)
            return 1
        image = image_path.read_bytes()
        if merged[offset : offset + len(image)] != image:
            print(f"ERROR: {relative_name} differs at merged offset 0x{offset:x}", file=sys.stderr)
            return 1
        print(f"Verified {relative_name}: {len(image)} bytes at 0x{offset:x}")

    if len(merged) > 8 * 1024 * 1024:
        print("ERROR: merged firmware exceeds 8 MB", file=sys.stderr)
        return 1

    print(f"Merged firmware: PASS ({len(merged)} bytes, flash at 0x0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
