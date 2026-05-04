/**
 * file:        fsx492.h
 * description: fsx492 file system interface
 *              
 * credit:
 *  Peter Desnoyers, November 2016
 *  Philip Gust, March 2019
 *  Phillippe Meunier, 2020-2025
 *  Ryan Tsang, 2026
 */

#ifndef __FSX492_H__
#define __FSX492_H__

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

enum {
    FSX492_MAGIC    = 0xC492F11E,   // superblock magic number
    FSX492_BLKSZ    = 1024,         // block size in bytes
    FSX492_DIRENTSZ = 32,           // directory entry size in bytes
    FSX492_INODESZ  = 64,           // inode size in bytes
};

/**
 * @brief      directory entry struct
 */
enum {
    FSX492_FILENAMESZ = 28,     // max filename size (including `\0`)
};
struct fsx492_dirent {
    uint32_t valid : 1;             // entry valid bit
    uint32_t ino : 31;              // entry inode number
    char name[FSX492_FILENAMESZ];   // entry name
};
static_assert(sizeof(struct fsx492_dirent) == FSX492_DIRENTSZ);


/**
 * @brief      superblock struct
 */
struct fsx492_superblk {
    uint32_t magic;             // magic number
    uint32_t inode_map_sz;      // inode map size in blocks
    uint32_t block_map_sz;      // block map size in blocks
    uint32_t inode_region_sz;   // inode region size in blocks
    uint32_t total_blocks;      // total blocks in block device
    uint32_t root_inode;        // root inode index (always 1)

    // padding to fill block
    uint8_t __padding[FSX492_BLKSZ - 6 * sizeof(uint32_t)];
};
static_assert(sizeof(struct fsx492_superblk) == FSX492_BLKSZ);


/**
 * @brief      inode struct
 */
enum {
    FSX492_N_DIRECT = 6,        // number of direct block pointers
};
struct fsx492_inode {
    uint32_t ino;       // inode number
    uint32_t mode;      // mode bits (filetype + permissions)
    uint16_t uid;       // owner user id
    uint16_t gid;       // owner group id
    uint32_t size;      // file size in bytes
    uint16_t nlink;     // number of hard links
    uint16_t blocks;    // number of blocks allocated for file
    uint32_t atime;     // access time
    uint32_t ctime;     // creation time
    uint32_t mtime;     // modification time
    uint32_t direct_blks[FSX492_N_DIRECT];  // direct block pointers
    uint32_t indir1_blks;   // singly-indirect block pointer
    uint32_t indir2_blks;   // doubly-indirect block pointer
};
static_assert(sizeof(struct fsx492_inode) == FSX492_INODESZ);


/**
 * useful constants
 */
enum {
    // # directory entries per block
    FSX492_DIRENTRIES_PER_BLK = FSX492_BLKSZ / sizeof(struct fsx492_dirent),

    // # inodes per block
    FSX492_INODES_PER_BLK     = FSX492_BLKSZ / sizeof(struct fsx492_inode),
    
    // # inode or block pointers per block
    FSX492_PTRS_PER_BLK       = FSX492_BLKSZ / sizeof(uint32_t),
    
    // # bits per block
    FSX492_BITS_PER_BLK       = FSX492_BLKSZ * 8,
};


/** function prototypes */

void * fsx492_init(struct fuse_conn_info * conn, struct fuse_config * cfg);
void fsx492_destroy(void * private_data);
int fsx492_getattr(
    const char * path, struct stat * status, struct fuse_file_info * fi);
int fsx492_mknod(const char * path, mode_t mode, dev_t dev);
int fsx492_open(const char * path, struct fuse_file_info * fi);
int fsx492_read(const char * path, char * buf, size_t size, off_t offset,
    struct fuse_file_info * fi);
int fsx492_write(const char * path, const char * buf, size_t size,
    off_t offset, struct fuse_file_info * fi);
int fsx492_release(const char * path, struct fuse_file_info * fi);
int fsx492_mkdir(const char * path, mode_t mode);
int fsx492_opendir(const char * path, struct fuse_file_info * fi);
int fsx492_readdir(const char * path, void * buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int fsx492_releasedir(const char * path, struct fuse_file_info * fi);
int fsx492_link(const char * oldpath, const char * newpath);
int fsx492_unlink(const char * path);
int fsx492_rmdir(const char * path);
int fsx492_truncate(const char * path, off_t len, struct fuse_file_info *fi);
int fsx492_rename(
    const char * oldpath, const char * newpath, unsigned int flags);
int fsx492_chmod(const char * path, mode_t mode, struct fuse_file_info * fi);
int fsx492_utimens(
    const char * path, const struct timespec tv[2], struct fuse_file_info *fi);
int fsx492_statfs(const char * path, struct statvfs * st);

#endif // __FSX492_H__