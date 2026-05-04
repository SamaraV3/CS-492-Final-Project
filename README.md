# FSX492

Group Members
- member1 (email1)
- member2 (email2)
- member3 (email3)

Additional features implemented:

- [ ] access control
- [ ] symlinks

# Implemented Functions

## `blkdev.c`

These functions simulate disk operations.
They are critical for anything to work correctly.

- [ ] `blkdev_read`
- [ ] `blkdev_write`
- [ ] `blkdev_flush`
- [ ] `blkdev_close`

## `fsx492.c`

### Helpers

These are important helper functions.

- [ ] `search_block` (optional if not needed)
- [ ] `find_entry`
- [ ] `_link`
- [ ] `_unlink`

### FUSE Callbacks

These are the FUSE callbacks that need to be implemented.
(All FUSE callbacks that currently return `-ENOSYS`)

They are listed in suggested order of implementation.

- [ ] `fsx492_getattr` (high priority)
- [ ] `fsx492_opendir` (high priority)
- [ ] `fsx492_releasedir` (high priority)
- [ ] `fsx492_mkdir`
- [ ] `fsx492_rmdir`
- [ ] `fsx492_open`
- [ ] `fsx492_release`
- [ ] `fsx492_write` (difficult)
- [ ] `fsx492_link`
- [ ] `fsx492_chmod`

For additional features:
(Note that these functions do not exist in the template, you must add them.)

- [ ] `fsx492_access` (access control)
- [ ] `fsx492_symlink` (symlinks)



## `test.py`

You must write tests that exercise all FUSE callbacks.
See the `test.py` file for examples of how these can be written.

You should create tests for at least the following conditions:

- [ ] adding and removing files from subdirectories
- [ ] adding and removing more than a block's worth of directories (at once)
- [ ] overwriting a file (see `open` behavior)
- [ ] opening a file in "append" mode (see `open` behavior)
- [ ] counting hard links
- [ ] update access/modification time
- [ ] changing permissions

If you implement permissions checking:

- [ ] expect error when opening a write-only file for reading
- [ ] expect error when opening a read-only file for writing
- [ ] expect error when reading from a file opened as write-only
- [ ] expect error when writing to a file opened as read-only

If you implement symlinks:

- [ ] resolve symlink to file and directory
- [ ] read and write to file via symlink
- [ ] link/unlink directory entries via symlink
- [ ] resolve symlinks to symlinks


# Helpful Links

- [Geoff Kuenning's (Harvey Mudd) FUSE Documentation](https://www.cs.hmc.edu/~geoff/classes/hmc.cs135.201109/homework/fuse/fuse_doc.html)
- [libfuse source](https://github.com/libfuse/libfuse)
- [libfuse documentation](https://libfuse.github.io/doxygen/index.html)
  - [libfuse operations documentation](https://libfuse.github.io/doxygen/structfuse__operations.html)
- [Open Group Base Specifications Issue 8](https://pubs.opengroup.org/onlinepubs/9799919799/)
  - [Open Group Base Spec Headers](https://pubs.opengroup.org/onlinepubs/9799919799/idx/head.html)
- [linux man-pages](https://man7.org/linux/man-pages/)
  - [select(2)](https://man7.org/linux/man-pages/man2/select.2.html) (for `fd_set`)
- [C standard reference](https://en.cppreference.com/c)


# ImHex Helper Script

If you would like to use [ImHex](https://docs.werwolv.net/imhex), here is a basic script that highlights the superblock, the inodes, and the first block of entries in the root directory.

```imhex
struct superblock {
    u32 magic;
    u32 inode_map_sz;
    u32 block_map_sz;
    u32 inode_region_sz;
    u32 total_blocks;
    u32 root_inode;
    char __padding[1024 - 6 * 4];
};

struct inode {
    u32 ino;
    u32 mode;
    u16 uid;
    u16 gid;
    u32 size;
    u16 nlink;
    u16 blocks;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 direct[6];
    u32 indir1;
    u32 indir2;
};

struct dirent {
    u32 inode;
    char name[28];
};

superblock sb @ 0x0;
inode inodes[64] @ 0x800;
dirent root[32] @ 0x1c00;
```

