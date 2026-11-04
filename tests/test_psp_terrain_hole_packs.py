import contextlib
import io
from pathlib import Path
import runpy
import struct
import tempfile
import unittest

tools = runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/psp/build_terrain_hole_overrides.py"))


def adt(masks):
    data = bytearray(struct.pack("<4sII", b"REVM", 4, 18))
    for i, mask in enumerate(masks):
        head = bytearray(128)
        struct.pack_into("<II", head, 4, i % 16, i // 16)
        struct.pack_into("<H", head, 60, mask)
        data += struct.pack("<4sI", b"KNCM", len(head)) + head
    return data


class TerrainHolePacks(unittest.TestCase):
    def test_pack_matches_adt_and_excludes_solid_tiles(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            maps = root / "extracted/world/maps/azeroth"
            maps.mkdir(parents=True)
            masks = [0] * 256
            masks[17] = 0x10
            masks[216] = 0xFFFF
            source = maps / "azeroth_31_28.adt"
            source.write_bytes(adt(masks))
            (maps / "azeroth_30_28.adt").write_bytes(adt([0] * 256))
            self.assertEqual(tools["read_masks"](source), masks)
            with contextlib.redirect_stdout(io.StringIO()):
                summary = tools["build"](root / "extracted", root / "data")
            self.assertEqual(len(summary), 1)
            pack = (root / "data/terrain_holes_azeroth.pak").read_bytes()
            magic, version, count, toc = struct.unpack_from("<4sIII", pack)
            self.assertEqual((magic, version, count, toc), (b"WPAK", 1, 1, 16))
            name, offset, size, kind, reserved = struct.unpack_from("<48sIIII", pack, toc)
            self.assertEqual(name.rstrip(b"\0"), b"t_31_28")
            self.assertEqual((offset % 64, size, kind, reserved), (0, 512, 3, 0))
            self.assertEqual(list(struct.unpack_from("<256H", pack, offset)), masks)

    def test_invalid_adt_is_reported_without_guessing_holes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "broken.adt"
            for data in (b"", b"KNCM", struct.pack("<4sI", b"KNCM", 500), adt([0])):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    tools["read_masks"](path)

    def test_clear_tiles_do_not_create_override_packs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "world/maps/azeroth"
            source.mkdir(parents=True)
            (source / "azeroth_31_28.adt").write_bytes(adt([0] * 256))
            self.assertEqual(tools["build"](root, root / "out"), [])
            self.assertFalse((root / "out/terrain_holes_azeroth.pak").exists())


if __name__ == "__main__":
    unittest.main()
