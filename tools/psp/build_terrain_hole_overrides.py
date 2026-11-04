#!/usr/bin/env python3
"""Recover original MCNK holes for older PSP terrain packs, without rebaking textures."""
import argparse
import pathlib
import struct


def read_masks(path):
    """Use the same sequential MCNK order as ADTLoader and the WTER baker."""
    masks = []
    with path.open("rb") as source:
        length = source.seek(0, 2)
        source.seek(0)
        while source.tell() < length:
            raw = source.read(8)
            if len(raw) != 8:
                raise ValueError(f"{path}: truncated ADT chunk header")
            tag, size = struct.unpack("<4sI", raw)
            end = source.tell() + size
            if end > length:
                raise ValueError(f"{path}: chunk exceeds file")
            if tag in (b"KNCM", b"MCNK"):
                if size < 128:
                    raise ValueError(f"{path}: truncated MCNK")
                head = source.read(128)
                masks.append(struct.unpack_from("<H", head, 60)[0])
            source.seek(end)
    if len(masks) != 256:
        raise ValueError(f"{path}: expected 256 MCNK chunks, found {len(masks)}")
    return masks


def write_pack(path, records):
    # Standard WPAK v1, sorted names, raw 512-byte little-endian mask records.
    names = sorted(records)
    if any(len(name.encode("ascii")) >= 48 for name in names):
        raise ValueError("WPAK entry name too long")
    offset = (16 + 64 * len(names) + 63) & ~63
    temporary = path.with_suffix(".pak.tmp")
    path.parent.mkdir(parents=True, exist_ok=True)
    with temporary.open("wb") as dest:
        dest.write(struct.pack("<4sIII", b"WPAK", 1, len(names), 16))
        for i, name in enumerate(names):
            dest.write(struct.pack("<48sIIII", name.encode("ascii"), offset + i * 512, 512, 3, 0))
        dest.write(bytes(offset - dest.tell()))
        for name in names:
            dest.write(struct.pack("<256H", *records[name]))
    temporary.replace(path)


def build(extracted, output, maps=None):
    root = extracted / "world" / "maps"
    if not root.is_dir():
        raise ValueError(f"Extracted world/maps folder is missing: {root}")
    summary = []
    warnings = []
    for folder in sorted(root.iterdir()):
        if not folder.is_dir() or (maps and folder.name.lower() not in maps):
            continue
        records = {}
        total = 0
        for path in sorted(folder.glob("*.adt")):
            parts = path.stem.rsplit("_", 2)
            if len(parts) != 3 or not parts[-2].isdigit() or not parts[-1].isdigit():
                continue
            try:
                masks = read_masks(path)
            except ValueError as error:
                warnings.append(str(error))
                continue
            total += 1
            if any(masks):
                records[f"t_{int(parts[-2])}_{int(parts[-1])}"] = masks
        if records:
            target = output / f"terrain_holes_{folder.name.lower()}.pak"
            write_pack(target, records)
            chunks = sum(bool(mask) for masks in records.values() for mask in masks)
            print(f"{folder.name}: {len(records)}/{total} tiles, {chunks} hole chunks, {target.stat().st_size} bytes", flush=True)
            summary.append((folder.name, len(records), chunks, target.stat().st_size))
    if warnings:
        output.mkdir(parents=True, exist_ok=True)
        (output / "terrain_holes_warnings.txt").write_text("\n".join(warnings) + "\n", encoding="utf-8")
        print(f"Skipped {len(warnings)} empty/invalid ADTs; see terrain_holes_warnings.txt.", flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extracted", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True, help="Output data directory")
    parser.add_argument("--map", action="append", dest="maps")
    args = parser.parse_args()
    result = build(args.extracted, args.out, {m.lower() for m in args.maps} if args.maps else None)
    if not result:
        raise SystemExit("No terrain holes found. Check the extracted client folder.")
    print(f"Generated {len(result)} map packs. Copy them into data/ beside terrain_<map>.pak.")


if __name__ == "__main__":
    main()
