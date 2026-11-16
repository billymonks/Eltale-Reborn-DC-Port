#!/usr/bin/env python3
"""Copies Controller Pak saves from the PC recomp onto a Flycast VMU image.

Usage: pc_saves_to_vmu.py <vmu image> [--disc eltale|quest64|gameplay] [--from <saves dir>]
                          [--slots 0,3,...] [--list]

The PC pak is controllerPak_header.sav plus controllerPak_file_<n>.sav in the saves folder
(default %LOCALAPPDATA%\\EltaleReborn-PC\\saves for Eltale, Quest64Recompiled\\saves for Quest 64).
Its files become the disc's VMU file (Q64ELT.PAK, QUEST64.PAK or Q64GP.PAK, as runtime/src/pak.c
writes it), replacing the one on the image; other files on the image are left alone. The image is
copied to <image>.bak first. With no image there, a blank formatted one is made.

Flycast for Windows keeps controller A's first VMU in data\\vmu_save_A1.bin next to flycast.exe
(<serial>_vmu_save_A1.bin with per-game VMUs). Close Flycast before converting, or it writes its
copy back over the new one.

--slots picks pak slots (0-15) instead of every file. --list shows what's on the image and exits.

Pak files whose names collide (the PC keeps every file from before its Oct 30 pak fix as save
"0") are renamed to save <slot>, so the Dreamcast port can tell them apart.
"""
import datetime
import os
import shutil
import struct
import sys

DISCS = {
    "quest64": ("QUEST64.PAK", "Quest 64", "Quest64Recompiled"),
    "eltale": ("Q64ELT.PAK", "Eltale Reborn", "EltaleReborn-PC"),
    "gameplay": ("Q64GP.PAK", "Quest 64 GP", None),
}

PAK_MAX_FILES = 16
PAK_PAGE_SIZE = 256
PAK_FREE_PAGES = 123
IMAGE_MAGIC = b"Q64PAK01"

BLOCK = 512
BLOCKS = 256
ROOT, FAT = 255, 254
DIR_LOC, DIR_SIZE = 253, 13
USER_BLOCKS = 200
FAT_FREE, FAT_END = 0xFFFC, 0xFFFA


# --- PC pak ---

def unswap_name(stored):
    """Header names are stored with each 4-byte group reversed."""
    return bytes(stored[i ^ 3] for i in range(len(stored)))


def read_pc_pak(saves_dir, slots):
    header_path = os.path.join(saves_dir, "controllerPak_header.sav")
    with open(header_path, "rb") as f:
        header = f.read()
    files = []
    for slot in range(PAK_MAX_FILES):
        if slots is not None and slot not in slots:
            continue
        entry = header[slot * 32:slot * 32 + 32].ljust(32, b"\0")
        size, game, company = struct.unpack_from("<IIH", entry)
        path = os.path.join(saves_dir, "controllerPak_file_%d.sav" % slot)
        if company == 0 or game == 0 or not os.path.exists(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        if size == 0 or size > PAK_FREE_PAGES * PAK_PAGE_SIZE:
            size = len(data)
        size = (size + PAK_PAGE_SIZE - 1) // PAK_PAGE_SIZE * PAK_PAGE_SIZE
        files.append({
            "slot": slot,
            "company": company,
            "game": game,
            "name": unswap_name(entry[0x10:0x20]),
            "ext": unswap_name(entry[0x0C:0x10]),
            "data": data[:size].ljust(size, b"\0"),
            "mtime": os.path.getmtime(path),
        })
    return files


def fix_duplicate_names(files):
    keys = [(f["company"], f["game"], f["name"], f["ext"]) for f in files]
    if len(set(keys)) == len(keys):
        return False
    for f in files:
        f["ext"] = bytes([0x10 + f["slot"], 0, 0, 0])
    return True


# --- Q64PAK01 image and VMU package (runtime/src/pak.c) ---

def build_image(files):
    out = bytearray(IMAGE_MAGIC + struct.pack(">I", len(files)))
    for f in files:
        out += struct.pack(">III", f["slot"], f["company"], f["game"])
        out += f["name"] + f["ext"] + struct.pack(">I", len(f["data"])) + f["data"]
    return bytes(out)


def parse_image(img):
    if img[:8] != IMAGE_MAGIC:
        return None
    files, p = [], 12
    for _ in range(struct.unpack_from(">I", img, 8)[0]):
        slot, company, game = struct.unpack_from(">III", img, p)
        size = struct.unpack_from(">I", img, p + 32)[0]
        files.append({"slot": slot, "company": company, "game": game, "name": img[p + 12:p + 28],
                      "ext": img[p + 28:p + 32], "data": img[p + 36:p + 36 + size]})
        p += 36 + size
    return files


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def draw_icon():
    """The pearl in a gold ring, as draw_icon() in pak.c draws it (float32 math)."""
    pal = [0x0000, 0xF430, 0xF750, 0xFA80, 0xFDB2, 0xF211, 0xF433, 0xF755, 0xFA88, 0xFDCC, 0xFFFF]
    icon = bytearray(512)
    for y in range(32):
        for x in range(32):
            dx, dy = f32(x - 15.5), f32(y - 15.5)
            r = f32(f32(dx * dx + dy * dy) ** 0.5)
            c = 0
            if r < 10.5:
                lx, ly = f32(dx + 4.0), f32(dy + 4.0)
                d = f32(f32(lx * lx + ly * ly) ** 0.5)
                c = 10 if d < 2.5 else 9 - int(f32(d / 3.5))
                c = max(c, 5)
            elif r < 15.5:
                shade = f32(f32(dx + dy) / 22.0)
                c = 3 - int(f32(f32(shade * 2.0) + (1.0 if (r < 12.5 or r > 14.5) else 0.0)))
                c = min(max(c, 1), 4)
            i = (y * 32 + x) // 2
            icon[i] = (icon[i] & 0xF0) | c if x & 1 else (icon[i] & 0x0F) | (c << 4)
    return struct.pack("<16H", *(pal + [0] * (16 - len(pal)))), bytes(icon)


def vms_crc(buf):
    n = 0
    for b in buf:
        n ^= b << 8
        for _ in range(8):
            n = ((n << 1) ^ 0x1021) if n & 0x8000 else n << 1
    return n & 0xFFFF


def build_package(desc, data):
    pal, icon = draw_icon()
    hdr = bytearray(0x60)
    hdr[0x00:0x10] = desc.encode().ljust(16, b" ")[:16]
    hdr[0x10:0x30] = (desc + " saves").encode().ljust(32, b" ")[:32]
    hdr[0x30:0x40] = b"Quest64-DC-Port".ljust(16, b"\0")
    struct.pack_into("<HHHHI", hdr, 0x40, 1, 0, 0, 0, len(data))
    pkg = bytearray(hdr + pal + icon + data)
    struct.pack_into("<H", pkg, 0x46, vms_crc(pkg))
    return bytes(pkg)


def package_data(pkg):
    icons, _, eyecatch, _, size = struct.unpack_from("<HHHHI", pkg, 0x40)
    start = 0x80 + 512 * icons + {0: 0, 1: 8064, 2: 4544, 3: 2048}.get(eyecatch, 0)
    return pkg[start:start + size]


# --- VMU filesystem ---

def blank_vmu():
    vmu = bytearray(BLOCKS * BLOCK)
    root = bytearray(BLOCK)
    root[0:16] = b"\x55" * 16
    root[0x10:0x15] = b"\x01\xff\xff\xff\x64"
    root[0x30:0x38] = bytes.fromhex("1998112700005904")
    struct.pack_into("<HHHHHHHHHH", root, 0x40, 0xFF, 0, 0xFF, FAT, 1, DIR_LOC, DIR_SIZE, 5, USER_BLOCKS, 0x1F)
    struct.pack_into("<H", root, 0x56, 0x80)
    vmu[ROOT * BLOCK:] = root
    fat = [FAT_FREE] * BLOCKS
    fat[ROOT] = fat[FAT] = FAT_END
    for b in range(DIR_LOC, DIR_LOC - DIR_SIZE, -1):
        fat[b] = b - 1
    fat[DIR_LOC - DIR_SIZE + 1] = FAT_END
    vmu[FAT * BLOCK:(FAT + 1) * BLOCK] = struct.pack("<256H", *fat)
    return vmu


class Vmu:
    def __init__(self, raw):
        self.raw = bytearray(raw)
        root = self.raw[ROOT * BLOCK:]
        self.fat_loc, _, self.dir_loc, self.dir_size, _, self.user_blocks = struct.unpack_from("<6H", root, 0x46)
        self.fat = list(struct.unpack_from("<256H", self.raw, self.fat_loc * BLOCK))

    def entries(self):
        for n in range(self.dir_size):
            for e in range(16):
                off = (self.dir_loc - n) * BLOCK + e * 32
                yield off, self.raw[off:off + 32]

    def find(self, name):
        for off, ent in self.entries():
            if ent[0] and ent[4:16].rstrip(b"\0 ") == name.encode():
                return off
        return None

    def read(self, off):
        b, out = struct.unpack_from("<H", self.raw, off + 2)[0], bytearray()
        while b < BLOCKS and len(out) < BLOCKS * BLOCK:
            out += self.raw[b * BLOCK:(b + 1) * BLOCK]
            b = self.fat[b]
        return bytes(out)

    def free_blocks(self):
        return sum(1 for b in range(self.user_blocks) if self.fat[b] == FAT_FREE)

    def delete(self, off):
        b = struct.unpack_from("<H", self.raw, off + 2)[0]
        while b < BLOCKS:
            next_block = self.fat[b]
            self.fat[b] = FAT_FREE
            b = next_block
        self.raw[off:off + 32] = bytes(32)

    def write(self, name, data):
        """Writes a data file from the top of the user blocks down, like KOS's vmufs."""
        count = (len(data) + BLOCK - 1) // BLOCK
        old = self.find(name)
        if old is not None:
            self.delete(old)
        free = [b for b in range(self.user_blocks - 1, -1, -1) if self.fat[b] == FAT_FREE][:count]
        if len(free) < count:
            raise SystemExit("not enough room on the VMU: %s needs %d blocks, %d free" %
                             (name, count, self.free_blocks()))
        off = old if old is not None else next((o for o, e in self.entries() if e[0] == 0), None)
        if off is None:
            raise SystemExit("the VMU's directory is full")
        data = data.ljust(count * BLOCK, b"\0")
        for i, b in enumerate(free):
            self.raw[b * BLOCK:(b + 1) * BLOCK] = data[i * BLOCK:(i + 1) * BLOCK]
            self.fat[b] = free[i + 1] if i + 1 < count else FAT_END
        bcd = lambda v: ((v // 10) << 4) | (v % 10)
        now = datetime.datetime.now()
        ent = bytearray(32)
        ent[0] = 0x33
        struct.pack_into("<H", ent, 2, free[0])
        ent[4:16] = name.encode().ljust(12, b"\0")
        ent[0x10:0x18] = bytes(bcd(v) for v in (now.year // 100, now.year % 100, now.month, now.day,
                                               now.hour, now.minute, now.second, now.weekday()))
        struct.pack_into("<HH", ent, 0x18, count, 0)
        self.raw[off:off + 32] = ent

    def save(self):
        self.raw[self.fat_loc * BLOCK:(self.fat_loc + 1) * BLOCK] = struct.pack("<256H", *self.fat)
        return bytes(self.raw)


# --- Output ---

def pak_char(c):
    """The Controller Pak's character set: 0x10-0x19 digits, 0x1A-0x33 letters."""
    if 0x10 <= c <= 0x19:
        return chr(ord("0") + c - 0x10)
    if 0x1A <= c <= 0x33:
        return chr(ord("A") + c - 0x1A)
    return {0x0F: " ", 0x00: ""}.get(c, "?")


def describe(f):
    name = "".join(pak_char(c) for c in f["name"])
    ext = "".join(pak_char(c) for c in f["ext"])
    line = "  slot %2d  %s.%s  %d bytes" % (f["slot"], name, ext or "-", len(f["data"]))
    if "mtime" in f:
        line += "  (PC file from %s)" % datetime.datetime.fromtimestamp(f["mtime"]).strftime("%Y-%m-%d %H:%M")
    return line


def list_vmu(vmu):
    print("%d of %d blocks free" % (vmu.free_blocks(), vmu.user_blocks))
    for off, ent in vmu.entries():
        if not ent[0]:
            continue
        name = ent[4:16].rstrip(b"\0 ").decode("ascii", "replace")
        print("%-12s %3d blocks" % (name, struct.unpack_from("<H", ent, 0x18)[0]))
        files = parse_image(package_data(vmu.read(off))) if name.endswith(".PAK") else None
        for f in files or []:
            print(describe(f))


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 1
    vmu_path = args.pop(0)
    disc, saves_dir, slots, list_only = "eltale", None, None, False
    while args:
        a = args.pop(0)
        if a == "--disc":
            disc = args.pop(0)
        elif a == "--from":
            saves_dir = args.pop(0)
        elif a == "--slots":
            slots = {int(s) for s in args.pop(0).split(",")}
        elif a == "--list":
            list_only = True
        else:
            print("unknown option %s" % a)
            return 1
    if disc not in DISCS:
        print("--disc is one of %s" % ", ".join(DISCS))
        return 1
    file_name, desc, pc_folder = DISCS[disc]

    if os.path.exists(vmu_path):
        with open(vmu_path, "rb") as f:
            vmu = Vmu(f.read())
    else:
        vmu = Vmu(blank_vmu())
    if list_only:
        list_vmu(vmu)
        return 0

    if saves_dir is None:
        if pc_folder is None:
            print("--from is needed for --disc %s" % disc)
            return 1
        saves_dir = os.path.join(os.environ.get("LOCALAPPDATA", ""), pc_folder, "saves")
    files = read_pc_pak(saves_dir, slots)
    if not files:
        print("no pak files in %s" % saves_dir)
        return 1
    if fix_duplicate_names(files):
        print("The PC pak gives several files the same name; renamed each to its slot number.")

    pkg = build_package(desc, build_image(files))
    vmu.write(file_name, pkg)
    if os.path.exists(vmu_path):
        shutil.copyfile(vmu_path, vmu_path + ".bak")
    os.makedirs(os.path.dirname(os.path.abspath(vmu_path)), exist_ok=True)
    with open(vmu_path, "wb") as f:
        f.write(vmu.save())
    print("Wrote %s (%d blocks, %d files) to %s:" % (file_name, (len(pkg) + BLOCK - 1) // BLOCK, len(files), vmu_path))
    for f in files:
        print(describe(f))
    return 0


if __name__ == "__main__":
    sys.exit(main())
