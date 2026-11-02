"""Tiberian Dawn's mix files read on the host (docs/games/tdawn.md, "Tiberian Dawn in 3D").

A mix of the 1995 game is a count (16 bits), the bytes of its files (32 bits), then a record a
file, sorted by id: the id (a sum of the upper-case name, the game's CRCEngine), where its bytes
begin after the records, how many. The demo's are not encrypted. Nothing here is the game's
code: the format and the sum are what the files hold.
"""
import struct


def name_id(name):
    """The id a mix knows a file by: four bytes at a time, the sum turned left one and added."""
    data = name.upper().encode("ascii")
    crc = 0
    for at in range(0, len(data), 4):
        piece = data[at:at + 4].ljust(4, b"\0")
        crc = (((crc << 1) | (crc >> 31)) & 0xFFFFFFFF)
        crc = (crc + struct.unpack("<I", piece)[0]) & 0xFFFFFFFF
    return crc


class Mix:
    def __init__(self, data):
        self.data = data
        count, size = struct.unpack_from("<HI", data, 0)
        if 6 + 12 * count > len(data):
            raise ValueError("not a mix of the 1995 game (or an encrypted one)")
        self.base = 6 + 12 * count
        self.files = {}
        for i in range(count):
            fid, offset, length = struct.unpack_from("<III", data, 6 + 12 * i)
            self.files[fid] = (offset, length)
        if self.base + size > len(data):
            raise ValueError("a mix shorter than its header says")

    @classmethod
    def open(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def get(self, name):
        """A file's bytes by its name, or None."""
        found = self.files.get(name_id(name))
        if not found:
            return None
        offset, length = found
        return self.data[self.base + offset:self.base + offset + length]

    def __contains__(self, name):
        return name_id(name) in self.files


def find(mixes, name):
    """A file in the first of these mixes, or in a mix inside one of them (THEATER.MIX)."""
    for m in mixes:
        got = m.get(name)
        if got is not None:
            return got
    return None
