#!/usr/bin/env python3
import struct
import time
import json
import argparse

# a vibe-coded image generation utility

# ---------------- Constants ----------------
FSX492_MAGIC = 0xC492F11E
FSX492_BLKSZ = 1024
FSX492_DIRENTSZ = 32
FSX492_INODESZ = 64

FSX492_FILENAMESZ = 28
FSX492_N_DIRECT = 6

INODES_PER_BLOCK = FSX492_BLKSZ // FSX492_INODESZ
DIRENTS_PER_BLOCK = FSX492_BLKSZ // FSX492_DIRENTSZ

# ---------------- Bitmaps ----------------
def set_bit(bm, i): bm[i // 8] |= (1 << (i % 8))
def test_bit(bm, i): return (bm[i // 8] >> (i % 8)) & 1

def alloc_bit(bm):
    for i in range(len(bm) * 8):
        if not test_bit(bm, i):
            set_bit(bm, i)
            return i
    raise RuntimeError("Out of space")

# ---------------- Dirent ----------------
class Dirent:
    FORMAT = "<I28s"

    def __init__(self, valid, ino, name):
        self.valid = valid
        self.ino = ino
        self.name = name

    def pack(self):
        vi = (self.valid & 1) | (self.ino << 1)
        name = self.name.encode()[:FSX492_FILENAMESZ]
        name = name.ljust(FSX492_FILENAMESZ, b"\x00")
        return struct.pack(self.FORMAT, vi, name)

    @classmethod
    def unpack(cls, data):
        vi, name = struct.unpack(cls.FORMAT, data)
        return cls(vi & 1, vi >> 1, name.rstrip(b"\x00").decode())

# ---------------- Superblock ----------------
class Superblock:
    FORMAT = "<6I"

    def __init__(self, inode_map_sz, block_map_sz, inode_region_sz, total_blocks):
        self.magic = FSX492_MAGIC
        self.inode_map_sz = inode_map_sz
        self.block_map_sz = block_map_sz
        self.inode_region_sz = inode_region_sz
        self.total_blocks = total_blocks
        self.root_inode = 1

    def pack(self):
        hdr = struct.pack(
            self.FORMAT,
            self.magic,
            self.inode_map_sz,
            self.block_map_sz,
            self.inode_region_sz,
            self.total_blocks,
            self.root_inode,
        )
        return hdr + bytes(FSX492_BLKSZ - len(hdr))

# ---------------- Inode ----------------
class Inode:
    FORMAT = "<IIHHIHHIII6I2I"

    def __init__(self, ino, mode):
        now = int(time.time())
        self.ino = ino
        self.mode = mode
        self.uid = 0
        self.gid = 0
        self.size = 0
        self.nlink = 1
        self.blocks = 0
        self.atime = now
        self.ctime = now
        self.mtime = now
        self.direct = [0] * FSX492_N_DIRECT
        self.indir1 = 0
        self.indir2 = 0

    def pack(self):
        return struct.pack(
            self.FORMAT,
            self.ino,
            self.mode,
            self.uid,
            self.gid,
            self.size,
            self.nlink,
            self.blocks,
            self.atime,
            self.ctime,
            self.mtime,
            *self.direct,
            self.indir1,
            self.indir2,
        )

# ---------------- FS Builder ----------------
class FS:
    def __init__(self, total_blocks=1024):
        self.total_blocks = total_blocks

        self.inode_map_sz = 1
        self.block_map_sz = 1
        self.inode_region_sz = 4

        self.sb = Superblock(
            self.inode_map_sz,
            self.block_map_sz,
            self.inode_region_sz,
            total_blocks,
        )

        self.inode_bitmap = bytearray(FSX492_BLKSZ)
        self.block_bitmap = bytearray(FSX492_BLKSZ)

        self.inodes = {}
        self.data = {}

        # reserve metadata blocks
        self.data_start = 1 + self.inode_map_sz + self.block_map_sz + self.inode_region_sz
        for i in range(self.data_start):
            set_bit(self.block_bitmap, i)

        # reserve inode 0, root = 1
        set_bit(self.inode_bitmap, 0)

        root_ino = self.alloc_inode()
        self.root = self.create_dir(root_ino, root_ino)

    def alloc_inode(self):
        return alloc_bit(self.inode_bitmap)

    def alloc_block(self):
        return alloc_bit(self.block_bitmap)

    # ---- directory helpers ----
    def read_dirents(self, inode):
        block = inode.direct[0]
        raw = self.data[block]
        ents = []
        for i in range(DIRENTS_PER_BLOCK):
            chunk = raw[i*32:(i+1)*32]
            d = Dirent.unpack(chunk)
            if d.valid:
                ents.append(d)
        return ents

    def write_dirents(self, inode, entries):
        block = inode.direct[0]
        raw = b''.join(e.pack() for e in entries)
        raw = raw.ljust(FSX492_BLKSZ, b'\x00')
        self.data[block] = raw
        inode.size = len(entries) * FSX492_DIRENTSZ

    def add_dirent(self, dir_inode, child_ino, name):
        entries = self.read_dirents(dir_inode)
        entries.append(Dirent(1, child_ino, name))
        self.write_dirents(dir_inode, entries)

    # ---- creation ----
    def create_dir(self, ino, parent):
        inode = Inode(ino, 0o40755)
        block = self.alloc_block()

        inode.direct[0] = block
        inode.blocks = 1

        entries = [
            Dirent(1, ino, "."),
            Dirent(1, parent, ".."),
        ]

        self.data[block] = b''.join(e.pack() for e in entries).ljust(FSX492_BLKSZ, b'\x00')
        inode.size = len(entries) * FSX492_DIRENTSZ

        self.inodes[ino] = inode
        return inode

    def create_file(self, ino, content):
        inode = Inode(ino, 0o100644)
        block = self.alloc_block()

        data = content.encode()
        self.data[block] = data.ljust(FSX492_BLKSZ, b'\x00')

        inode.direct[0] = block
        inode.size = len(data)
        inode.blocks = 1

        self.inodes[ino] = inode
        return inode

    # ---- build tree ----
    def build(self, tree, parent_inode):
        for name, val in tree.items():
            ino = self.alloc_inode()

            if isinstance(val, dict):
                inode = self.create_dir(ino, parent_inode.ino)
                self.add_dirent(parent_inode, ino, name)
                self.build(val, inode)
            else:
                inode = self.create_file(ino, val)
                self.add_dirent(parent_inode, ino, name)

    # ---- write image ----
    def write(self, path):
        with open(path, "wb") as f:
            # superblock
            f.write(self.sb.pack())

            # bitmaps
            f.write(self.inode_bitmap)
            f.write(self.block_bitmap)

            # inode region
            total_inodes = self.inode_region_sz * INODES_PER_BLOCK
            for i in range(total_inodes):
                inode = self.inodes.get(i)
                if inode:
                    f.write(inode.pack())
                else:
                    f.write(bytes(FSX492_INODESZ))

            # data blocks
            for i in range(self.data_start, self.total_blocks):
                if i in self.data:
                    f.write(self.data[i])
                else:
                    f.write(bytes(FSX492_BLKSZ))

# ---------------- CLI ----------------
def cmd_create(args):
    with open(args.json) as f:
        tree = json.load(f)

    fs = FS(args.blocks)
    fs.build(tree, fs.root)
    fs.write(args.image)

def cmd_ls(args):
    with open(args.image, "rb") as f:
        sb = f.read(FSX492_BLKSZ)
        print("Image size:", len(f.read()))

def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers()

    c = sub.add_parser("create")
    c.add_argument("json")
    c.add_argument("image")
    c.add_argument("--blocks", type=int, default=1024)
    c.set_defaults(func=cmd_create)

    l = sub.add_parser("ls")
    l.add_argument("image")
    l.set_defaults(func=cmd_ls)

    args = p.parse_args()
    args.func(args)

if __name__ == "__main__":
    main()