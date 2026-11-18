#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the compiled flo discovery DTB, including resolved phandles."""

import argparse
import struct
import unittest
from pathlib import Path


def cells(data):
    if len(data) % 4:
        raise ValueError("unaligned cells")
    return struct.unpack(f">{len(data) // 4}I", data)


def strings(data):
    return tuple(part.decode() for part in data.rstrip(b"\0").split(b"\0"))


def read_dtb(path):
    blob = Path(path).read_bytes()
    magic, size, structs, names = struct.unpack_from(">4I", blob)
    if magic != 0xD00DFEED or size > len(blob):
        raise ValueError("invalid DTB")
    tree = {}
    stack = []
    pos = structs
    while True:
        token, = struct.unpack_from(">I", blob, pos)
        pos += 4
        if token == 9:
            return tree
        if token == 1:
            end = blob.index(b"\0", pos)
            stack.append(blob[pos:end].decode())
            pos = (end + 4) & ~3
            tree["/" + "/".join(x for x in stack if x)] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, offset = struct.unpack_from(">2I", blob, pos)
            pos += 8
            end = blob.index(b"\0", names + offset)
            name = blob[names + offset:end].decode()
            tree["/" + "/".join(x for x in stack if x)][name] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token != 4:
            raise ValueError(f"bad FDT token {token}")


class FloAudioDiscoveryDTB(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tree = read_dtb(args.dtb)
        cls.handles = {
            cells(props["phandle"])[0]: (path, props)
            for path, props in cls.tree.items() if "phandle" in props
        }

    def node(self, suffix):
        matches = [(path, props) for path, props in self.tree.items()
                   if path.endswith(suffix)]
        self.assertEqual(len(matches), 1, suffix)
        return matches[0][1]

    def test_supplies_reset_and_mclk(self):
        node = self.node("/tabla-discovery")
        self.assertEqual(strings(node["compatible"]),
                         ("asus,flo-tabla-discovery",))
        self.assertEqual(self.handles[cells(node["vdd-1p8-supply"])[0]][0]
                         .split("/")[-1], "s4")
        self.assertEqual(self.handles[cells(node["vdd-1p25-supply"])[0]][0]
                         .split("/")[-1], "l25")
        self.assertEqual([key for key in node if key.endswith("-supply")],
                         ["vdd-1p8-supply", "vdd-1p25-supply"])
        reset = cells(node["reset-gpios"])
        self.assertEqual(reset[1:], (34, 1))  # PM8921 GPIO34, active low.
        self.assertTrue(self.handles[reset[0]][0].endswith("/gpio@150"))
        mclk = cells(node["clocks"])
        self.assertEqual(mclk[1:], (23,))  # CODEC_I2S_SPKR_OSR_CLK.
        self.assertTrue(self.handles[mclk[0]][0].endswith(
            "/clock-controller@28000000"))
        self.assertEqual(strings(node["clock-names"]), ("mclk",))

    def test_controller_and_pins(self):
        node = self.node("/tabla-discovery")
        controller = self.handles[cells(node["slim-controller"])[0]][1]
        self.assertEqual(strings(controller["status"]), ("okay",))
        slim_pins = self.handles[cells(controller["pinctrl-0"])[0]][1]
        self.assertEqual(strings(slim_pins["pins"]), ("gpio40", "gpio41"))
        self.assertEqual(strings(slim_pins["function"]), ("slimbus",))
        mclk_pins = self.handles[cells(node["pinctrl-0"])[0]][1]
        self.assertEqual(strings(mclk_pins["pins"]), ("gpio39",))
        self.assertEqual(strings(mclk_pins["function"]),
                         ("codec_spkr_i2s",))
        self.assertEqual(strings(node["status"]), ("okay",))
        bam = self.node("/dma-controller@28084000")
        self.assertEqual(strings(bam["status"]), ("disabled",))

    def test_no_codec_or_audio_route(self):
        compatibles = [value for props in self.tree.values()
                       for key, value in props.items() if key == "compatible"]
        self.assertFalse(any(b"slim217," in value for value in compatibles))
        self.assertFalse(any(b"simple-audio-card" in value
                             for value in compatibles))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("dtb")
    args = parser.parse_args()
    unittest.main(argv=[__file__])
