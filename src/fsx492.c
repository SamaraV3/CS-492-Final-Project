/**
 * file:        fsx492.c
 * description: fsx492 file system implementation
 *              
 * credit:
 *  Peter Desnoyers, November 2016
 *  Philip Gust, March 2019
 *  Phillippe Meunier, 2020-2025
 *  Ryan Tsang, 2026
 */

#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fuse.h>

#include "blkdev.h"
#include "fsx492.h"



/**
 * the block device is initialized in `main.c` when the image path
 * is initially parsed (it is inaccessible by other means)
 */
extern struct blkdev * disk;

/**
 * @brief      file system context
 * 
 * @note       fuse makes context available through the `private_data` field
 *             of a `struct fuse_context`, which is accessible in all file 
 *             operations via `fuse_get_context()`.
 *             
 * @note       we should make use of the provided means of storing context,
 *             as fuse can behave in a multithreaded manner.
 */
struct context {
    uint32_t initialized : 1;       // if context has been initialized
    uint32_t __unused : 31;
    void *   metadata;              // pointer to all metadata (includes sb)
    fd_set * inode_map;             // inode bitmap (in metadata)
    uint32_t inode_map_base;        // block index of inode map
    uint32_t n_inodes;              // max number of inodes
    fd_set * block_map;             // block bitmap (in metadata)
    uint32_t block_map_base;        // block index of block map
    uint32_t n_blocks;              // max number of blocks
    struct fsx492_inode * inodes;   // inodes table (in metadata)
    uint32_t inodes_base;           // block index of inodes region
    fd_set * dirty_map;             // track dirty metadata blocks
    uint32_t n_metablks;            // number of metadata blocks
    uint32_t root_inode;            // root inode number (always 1)
    uint32_t data_base;             // block index of data region
};

// allocate context statically
static struct context * _ctx = &(struct context){
    .initialized = false,
    .inode_map = NULL,
    .inode_map_base = 0,
    .n_inodes = 0,
    .block_map = NULL,
    .block_map_base = 0,
    .n_blocks = 0,
    .inodes = NULL,
    .inodes_base = 0,
    .dirty_map = NULL,
    .n_metablks = 0,
    .root_inode = 1,
    .data_base = 0,
};


/**
 * @brief      file handle struct
 */
struct fh {
    uint32_t ino;   // inode number
    int flags;      // open/permission flags
};


/*****************************************************************************
 * HELPER FUNCTIONS
 *****************************************************************************/

// DISK ----------------------------------------------------------------------

static inline int disk_err_check(int result)
{
    switch (result) {
    case BLKDEV_SUCCESS:
        return 0;
    case BLKDEV_E_BADADDR:
        fprintf(stderr, "disk error: bad address\n");
        break;
    case BLKDEV_E_UNAVAIL:
        fprintf(stderr, "disk error: device unavailable\n");
        break;
    case BLKDEV_E_FAULT:
        fprintf(stderr, "disk error: internal error\n");
        break;
    case BLKDEV_E_BADDEV:
        fprintf(stderr, "disk error: bad device\n");
        break;
    }
    return -EIO;
}

static inline int read_blks(uint32_t start, uint32_t n, void * buf)
{
    return disk_err_check(disk->ops->read(disk, start, n, buf));
}

static inline int write_blks(uint32_t start, uint32_t n, void * buf)
{
    return disk_err_check(disk->ops->write(disk, start, n, buf));
}

/**
 * @brief      clear blocks
 *
 * @param[in]  start  The starting block
 * @param[in]  n      The number of blocks to clear
 *
 * @return     0 on success
 *             -EIO on disk error
 */
static inline int clear_blks(uint32_t start, uint32_t n)
{
    uint8_t buf[FSX492_BLKSZ] = { 0 };
    int ret = 0;
    while (n-- && !ret) {
        ret = write_blks(start++, 1, (void *)&buf);
    }
    return ret;
}

// SPACE MANAGEMENT -----------------------------------------------------------

/**
 * @brief      check if a block is allocated
 *
 * @param[in]  blkno  The block number
 * @param      ctx    The context
 *
 * @return     0        if block is allocated
 *             -EINVAL  if block is not allocated
 */
static inline int validate_block(uint32_t blkno, struct context * ctx)
{
    return (!blkno || !FD_ISSET(blkno, ctx->block_map)) ? -EINVAL : 0;
}


/**
 * @brief      allocate a block
 *             sets block map dirty, does not write back
 *
 * @param[out] blkno The allocated block number
 * @param      ctx   The file system context
 *
 * @return     0 on success
 *             -ENOSPC if no blocks available
 *             -EIO on disk error
 */
static inline int alloc_blk(uint32_t * blkno, struct context * ctx)
{
    assert(ctx);
    assert(blkno);
    for (int i = ctx->data_base; i < ctx->n_blocks; i++) {
        if (FD_ISSET(i, ctx->block_map)) continue;

        fprintf(stderr, "allocating block %d\n", i);
        
        if (clear_blks(i, 1) < 0) return -EIO;

        FD_SET(i, ctx->block_map);
        FD_SET(ctx->block_map_base, ctx->dirty_map);
        *blkno = i;
        return 0;
    }
    return -ENOSPC;
}


/**
 * @brief      free a block in the data region
 *             sets block map dirty, does not write back
 *             WARNING: does not check if block is actually allocated
 *
 * @param[in]  blkno  The block number
 * @param      ctx    The file system context
 */
static inline void free_blk(uint32_t blkno, struct context * ctx)
{
    assert(ctx);
    assert(blkno >= ctx->data_base); // cannot free metadata blocks
    FD_CLR(blkno, ctx->block_map);
    FD_SET(ctx->block_map_base, ctx->dirty_map);
}


static inline size_t count_avail_blks(struct context * ctx)
{
    assert(ctx);
    size_t available = 0;
    for (int i = 0; i < ctx->n_blocks; i++) {
        if (!FD_ISSET(i, ctx->block_map)) {
            available++;
        }
    }
    return available;
}


/**
 * @brief      check if an inode exists
 *
 * @param[in]  ino   The inode number
 * @param      ctx   The context
 *
 * @return     0        inode exists
 *             -EINVAL  inode does not exist or invalid
 */
static inline int validate_inode(uint32_t ino, struct context * ctx)
{
    return (!ino || !FD_ISSET(ino, ctx->inode_map)) ? -EINVAL : 0;
}

/**
 * @brief      allocates an inode
 *             sets inode map dirty, does not write back
 *
 * @param[out] ino   The destination for allocated inode number
 * @param      ctx   The context
 *
 * @return     0 on success
 *             -ENOSPC if no available inodes
 */
static inline int alloc_inode(uint32_t * ino, struct context * ctx)
{
    assert(ctx);
    for (int i = ctx->root_inode + 1; i < ctx->n_inodes; i++) {
        if (FD_ISSET(i, ctx->inode_map)) continue;

        fprintf(stderr, "allocating inode %d\n", i);

        FD_SET(i, ctx->inode_map);
        FD_SET(ctx->inode_map_base, ctx->dirty_map);
        *ino = i;
        return 0;
    }
    return -ENOSPC;
}


/**
 * @brief      free an allocated inode (must be truncated to 0 first)
 *             WARNING: does not check if inode is actually allocated
 *
 * @param[in]  ino   The inode number
 * @param      ctx   The context
 */
static inline void free_inode(uint32_t ino, struct context * ctx)
{
    assert(ctx);
    assert(ino > ctx->root_inode); // cannot free root inode
    assert(ctx->inodes[ino].blocks == 0); // must have no blocks
    FD_CLR(ino, ctx->inode_map);
    FD_SET(ctx->inode_map_base, ctx->dirty_map);
}


static inline size_t count_avail_inodes(struct context * ctx)
{
    assert(ctx);
    struct fsx492_superblk * sb = (struct fsx492_superblk *)ctx->metadata;
    size_t available = 0;
    for (int i = 2; i < FSX492_INODES_PER_BLK * sb->inode_region_sz; i++) {
        if (!FD_ISSET(i, ctx->inode_map)) {
            available++;
        }
    }
    return available;
}


/**
 * @brief      mark inodes region for inode as dirty
 *
 * @param[in]  ino   The modified inode number
 * @param      ctx   The context
 */
static inline void dirty_inode(uint32_t ino, struct context * ctx)
{
    assert(ctx);
    assert(ino);
    FD_SET(ctx->inodes_base + ino / FSX492_INODES_PER_BLK, ctx->dirty_map);
}


/**
 * @brief      attempt to free the last `n` data blocks in an array
 *             of pointers of length `len`
 *
 * @param      blks  The array of pointers
 * @param[in]  len   The length of the array of pointers
 * @param[in]  n     The number of data blocks to free
 * @param      ctx   The file system context
 *
 * @return     the number of blocks actually freed
 */
static inline size_t _free_last_blks(
    uint32_t blks[], size_t len, size_t n, struct context * ctx)
{
    assert(blks);
    size_t nfreed = 0;
    for (size_t i = 0; nfreed < n && i < len; i++) {
        size_t idx = (len - 1) - i;
        if (validate_block(blks[idx], ctx) == 0) {
            free_blk(blks[idx], ctx);
            blks[idx] = 0;
            nfreed++;
        }
    }
    return nfreed;
}

/**
 * @brief      attempt to free the last `n` data blocks from an inode's
 *             array of direct pointers
 *
 * @param      inode The inode pointer
 * @param[in]  n     The number of blocks to free
 * @param      ctx   The file system context
 *
 * @return     the number of blocks actually freed
 */
static inline size_t _free_last_direct_blks(
    struct fsx492_inode * inode, size_t n, struct context * ctx)
{
    assert(ctx);
    assert(inode);

    size_t nfreed = _free_last_blks(
        inode->direct_blks, FSX492_N_DIRECT, n, ctx);

    inode->blocks -= nfreed;
    dirty_inode(inode->ino, ctx);
    return nfreed;
}

/**
 * @brief      attempt to free the last `n` data blocks from an inode's
 *             singly-indirect pointer block
 *
 * @param      inode The inode pointer
 * @param[in]  n     The number of data blocks to free
 * @param      ctx   The file system context
 *
 * @return     the number of data blocks actually freed
 * 
 * @note       it is annoying to implement correct recovery for this,
 *             so just crash the program if a disk error occurs
 */
static inline size_t _free_last_indir1_blks(
    struct fsx492_inode * inode, size_t n, struct context * ctx)
{
    assert(ctx);
    assert(inode);
    assert(inode->blocks > FSX492_N_DIRECT);
    if (validate_block(inode->indir1_blks, ctx) < 0) {
        return 0;
    }

    uint32_t blks[FSX492_PTRS_PER_BLK];
    if (read_blks(inode->indir1_blks, 1, (void *)blks) < 0) {
        exit(1);
    }

    size_t nfreed = _free_last_blks(blks, FSX492_PTRS_PER_BLK, n, ctx);

    inode->blocks -= nfreed;
    dirty_inode(inode->ino, ctx);

    if (write_blks(inode->indir1_blks, 1, (void *)blks) < 0) {
        exit(1);
    }

    if (!blks[0]) {
        // if the first pointer is empty, the entire pointer block
        // should be empty and we should free it
        free_blk(inode->indir1_blks, ctx);
        inode->indir1_blks = 0;
    }

    return nfreed;
}


/**
 * @brief      attempt to free the last `n` data blocks from an inode's
 *             doubly-indirect pointer block
 *
 * @param      inode  The inode
 * @param[in]  n      The number of data blocks to free
 * @param      ctx    The context
 *
 * @return     the number of data blocks actually freed
 * 
 * @note       it is annoying to implement correct recovery for this,
 *             so just crash the program if a disk error occurs
 */
static inline size_t _free_last_indir2_blks(
    struct fsx492_inode * inode, size_t n, struct context * ctx)
{
    assert(ctx);
    assert(inode);
    assert(inode->blocks > FSX492_N_DIRECT + FSX492_PTRS_PER_BLK);
    if (validate_block(inode->indir2_blks, ctx) < 0) {
        return 0;
    }

    uint32_t blks2[FSX492_PTRS_PER_BLK];
    uint32_t blks1[FSX492_PTRS_PER_BLK];

    if (read_blks(inode->indir2_blks, 1, (void *)blks2) < 0) {
        exit(1);
    }

    size_t nfreed = 0;

    for (size_t i = 0; i < FSX492_PTRS_PER_BLK; i++) {
        // free blocks in reverse order
        size_t idx = (FSX492_PTRS_PER_BLK - 1) - i;
        if (validate_block(blks2[idx], ctx) < 0) {
            continue;
        }

        if (read_blks(blks2[idx], 1, (void *)blks1) < 0) {
            exit(1);
        }

        size_t mfreed = _free_last_blks(blks1, FSX492_PTRS_PER_BLK, n, ctx);

        nfreed += mfreed;
        inode->blocks -= mfreed;
        dirty_inode(inode->ino, ctx);

        if (write_blks(blks2[idx], 1, (void *)blks1) < 0) {
            exit(1);
        }

        if (!blks1[0]) {
            // if the first pointer is empty, the entire block
            // should be empty and should be freed
            free_blk(blks2[idx], ctx);
            blks2[idx] = 0;
        }
    }

    if (write_blks(inode->indir2_blks, 1, (void *)blks2) < 0) {
        exit(1);
    }

    if (!blks2[0]) {
        // if the first pointer is empty, the entire block
        // should be empty and should be freed
        free_blk(inode->indir2_blks, ctx);
        inode->indir2_blks = 0;
    }

    return nfreed;
}

// NAVIGATION -----------------------------------------------------------------

/**
 * @brief      searches for the given name in directory block entries
 *
 * @param[in]  name     The name
 * @param      entries  The pointer to the directory entries
 *
 * @return     the index of the entry if found
 *             -ENOENT if it is not present in the block
 *             -EIO on disk error
 */
static inline ssize_t search_block(
    const char * name, struct fsx492_dirent * entries)
{
    assert(name);
    assert(entries);

    // TODO:

    // find index of `name` parameter if found in `entries` array
    //iterate over every entry in curr block.
    //if entry valid and name matches, return index
    for (int i=0; i<FSX492_DIRENTRIES_PER_BLK; i++) {
        if (entries[i].valid && strcmp(entries[i].name, name)==0) {
            return i;//aka index
        }
    }
    

    return -ENOENT;
}

/**
 * @brief      searches for the given name in the directory inode
 *
 * @param[in]  name     The name
 * @param[in]  dir_ino  The directory inode number
 * @param[out] ino      The destination for the entry inode number
 * @param      ctx      The file system context
 *
 * @return     0 if the entry inode number exists
 *             -ENOENT if name or directory does not exist
 *             -ENOTDIR if dir_ino was not a directory inode
 *             -EIO on disk error
 *             -EINVAL if dir_ino is 0
 */
static int find_entry(
    const char * name, uint32_t dir_ino, uint32_t * ino, struct context * ctx)
{
    assert(ctx);
    assert(name);

    // TODO:

    // check if directory inode aint 0
    if (dir_ino == 0) {return -EINVAL;}
    //check if dis directory inode exists
    if (validate_inode(dir_ino, ctx) < 0) {
        fprintf(stderr, "Directory inode %u does not exist\n", dir_ino);
        return -ENOENT;
    }
    struct fsx492_inode * dir_inode = &ctx->inodes[dir_ino];//gets dir inode struct
    if (!S_ISDIR(dir_inode->mode)) {
        fprintf(stderr, "Inode %u is not a directory\n", dir_ino);
        return -ENOTDIR;
    }//checks its acc a dir lol

    //atp dir exists, is not 0, and is acc a dir. so search ts
    struct fsx492_dirent entries[FSX492_DIRENTRIES_PER_BLK];//to hold a block of dir entries
    memset(entries, 0, sizeof(entries));
    // search directory entries in direct_blks - aka N_DIRECT
    for (int i=0; i < FSX492_N_DIRECT; i++) {
        if (dir_inode->direct_blks[i] == 0 || validate_block(dir_inode->direct_blks[i], ctx) < 0) {continue;}//if block not valid or is superblock, skip
        //block is valid - get it
        uint32_t blk = dir_inode->direct_blks[i];
        if (read_blks(blk, 1, (void *)entries) < 0) {return -EIO;}//read block into entries array
        //now search via search_block for name
        ssize_t idx = search_block(name, entries);
        if (idx >= 0) {
            //found it!
            *ino = entries[idx].ino;
            fprintf(stderr, "Found entry %s in directory inode %u with inode %u\n", name, dir_ino, *ino);
            return 0;
        }
    }

    fprintf(stderr, "Entry %s not found in directory inode %u\n", name, dir_ino);
    return -ENOENT;//name dne in dir
}


/**
 * @brief      finds the target file or directory's inode
 *             as well as its parent directory's inode
 *
 * @param      path        The absolute path
 * @param[out] target_ino  The location to store target file/directory inode
 *                         (optional)
 * @param[out] parent_ino  The location to store parent directory inode
 *                         (optional)
 *
 * @return     0        on success
 *             -EIO     on failure to read disk
 *             -EINVAL  if any component of path is invalid (inode 0)
 *             -ENOENT  if any component of path does not exist
 *             -ENOTDIR if any intermediate component of path isn't a directory
 *             -EACCESS if permission denied (optional)
 * 
 * @note       path argument may contain "." or "..", which must be resolved
 * 
 * @note       if the path is root, "parent_ino" shall be set to root as well
 * 
 * @note       on errors, the last successfully found directory will be stored
 *             in parent_ino and target_ino will be set to -1 if the error
 *             occured in the target file's parent directory, and 0 otherwise
 *
 * @note       IMPORTANT: whether you choose to implement permissions checking
 *             or symlinks you WILL need to modify or rewrite this function
 */
static int lookup_path(
    const char * path, uint32_t * target_ino, uint32_t * parent_ino)
{
    int ret = 0;
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    assert(path[0] == '/'); // path should always be absolute

    // create a copy of path for use with strtok (destructive)
    char * _path = strdup(path);
    
    // character length of path will generally be larger than the number
    // of components in the path, which is at minimum 2 (for root)
    const size_t _path_comp_len = strlen(_path) / 2 + 2;
    char * tokens[_path_comp_len];
    memset(tokens, 0, _path_comp_len * sizeof(char *));
    tokens[0] = "/";
    size_t path_depth = 1;

    // parse path for tokens
    for (char * token = strtok(_path, "/"); token; token = strtok(NULL, "/")) {
        tokens[path_depth++] = token;
    }

    // create a stack array to represent path as sequence of inode numbers
    uint32_t ipath[_path_comp_len];
    memset(ipath, 0, (_path_comp_len) * sizeof(uint32_t));
    ipath[0] = ctx->root_inode;

    // depth points to the next level being considered
    // start at 1, because 0 is always root
    size_t depth = 1;
    uint32_t ino = 0;

    // attempt to construct inode path
    size_t i;
    for (i = 1; i < path_depth; i++) {
        if (!strcmp(tokens[i], ".")) {
            continue;
        }

        if (!strcmp(tokens[i], "..")) {
            ipath[depth] = 0;
            depth = (depth-1) ? depth-1 : depth; // account for root
            continue;
        }

        switch (ret = find_entry(tokens[i], ipath[depth-1], &ino, ctx)) {
        case 0:
            ipath[depth++] = ino;
            break;

        case -EIO:
        case -ENOENT:
        case -ENOTDIR:
        case -EINVAL:
            if (target_ino)
                *target_ino = (i == path_depth - 1) ? -1 : 0;
            if (parent_ino)
                *parent_ino = ipath[depth-1];
            goto _return;

        default:
            assert(0); // unreachable
        }
    }

    depth--;

    // found target file
    if (target_ino)
        *target_ino = ipath[depth];
    if (parent_ino)
        *parent_ino = ipath[(depth) ? depth - 1 : depth];

_return:
    free(_path);
    return ret;
}

static int lookup_path_follow(
    const char * path, uint32_t * target_ino, uint32_t * parent_ino, int depth)
{
    if (depth > 64) return -ELOOP;  // too many symlinks

    uint32_t ino = 0;
    if (lookup_path(path, &ino, parent_ino) < 0) return ret;

    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    if (!S_ISLNK(ctx->inodes[ino].mode)) {
        if (target_ino) *target_ino = ino;
        return 0;
    }

    // read the symlink target
    char blkbuf[FSX492_BLKSZ];
    if (read_blks(ctx->inodes[ino].direct_blks[0], 1, blkbuf) < 0) return -EIO;

    // recurse with the target path
    return lookup_path_follow(blkbuf, target_ino, parent_ino, depth + 1);
}

#define LP(X,Y,Z) lookup_path_follow(X,Y,Z,0)


/**
 * @brief      get a pointer to this path's basename
 *
 * @param[in]  path  The path
 *
 * @return     a pointer to this path's basename
 */
static const char * basename(const char * path)
{
    size_t offset = 0;
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            offset = i;
        }
    }
    return path + offset + 1;
}

// MISCELLANEOUS --------------------------------------------------------------

/**
 * @brief      copy stat info from inode to sb
 *
 * @param      inode  The pointer to the inode
 * @param      sb     The pointer to the stat struct to populate
 * 
 * @note       opengroup POSIX spec
 *             https://pubs.opengroup.org/onlinepubs/7908799/xsh/sysstat.h.html
 */
static void copy_stat(struct fsx492_inode * inode, struct stat * statbuf)
{
    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_ino = inode->ino;
    statbuf->st_uid = inode->uid;
    statbuf->st_gid = inode->gid;
    statbuf->st_mode = (mode_t)inode->mode;
    statbuf->st_atime = inode->atime;
    statbuf->st_ctime = inode->ctime;
    statbuf->st_mtime = inode->mtime;
    statbuf->st_size = inode->size;
    statbuf->st_nlink = inode->nlink;
    statbuf->st_blksize = FSX492_BLKSZ;
    // stat st_blocks is # of 512B blocks allocated
    statbuf->st_blocks = inode->blocks * 2;
}


/**
 * @brief      write back dirty metadata blocks
 *
 * @param      ctx   The context
 *
 * @return     0     on success
 *             -EIO  on disk error
 */
static int writeback_metadata(struct context * ctx)
{
    for (size_t i = 0; i < ctx->n_metablks; i++) {
        if (!FD_ISSET(i, ctx->dirty_map)) {
            continue;
        }
        void * blk = (void *)((uint8_t *)ctx->metadata + i * FSX492_BLKSZ);
        if (write_blks(i, 1, blk) < 0) {
            return -EIO;
        }
    }
    return 0;
}


/**
 * @brief      truncate inode to exactly len bytes
 *
 * @param[in]  ino   The inode to truncate
 * @param      ctx   The file system context
 *
 * @return     0        on success
 *             -EIO     on disk error
 *             
 * @note       this is a deceptively hard function to write,
 */
static int _truncate(uint32_t ino, off_t len, struct context * ctx)
{
    fprintf(stdout, "_truncate(%u, %ld)\n", ino, len);
    assert(ctx);
    assert(ino > ctx->root_inode);
    struct fsx492_inode * inode = &ctx->inodes[ino];

    // find first block to free
    size_t first_freed = (len / FSX492_BLKSZ) + !!(len % FSX492_BLKSZ);

    // calculate how many blocks to free
    size_t nfree = inode->blocks - first_freed;
    if (nfree < 0) {
        // desired length is longer than current length
        return 0;
    }

    size_t freed = 0;

    if (nfree && inode->blocks > FSX492_N_DIRECT + FSX492_PTRS_PER_BLK) {
        size_t nfreed = _free_last_indir2_blks(inode, nfree, ctx);
        freed += nfreed;
        nfree -= nfreed;
    }

    if (nfree && inode->blocks > FSX492_N_DIRECT) {
        size_t nfreed = _free_last_indir1_blks(inode, nfree, ctx);
        freed += nfreed;
        nfree -= nfreed;
    }

    if (nfree && inode->blocks > 0) {
        size_t nfreed = _free_last_direct_blks(inode, nfree, ctx);
        freed += nfree;
        nfree -= nfreed;
    }

    assert(nfree == 0);
    inode->size = len;
    dirty_inode(inode->ino, ctx);

    return 0;
}


/**
 * @brief      link an existing inode to a new entry in the directory
 *
 * @param[in]  name     The link name
 * @param[in]  ino      The inode to link
 * @param[in]  dir_ino  The directory's inode
 * @param      ctx      The file system context
 *
 * @return     0 on success
 *             -EIO on disk error
 *             -ENOSPC if directory full
 *             -EINVAL if name too long
 *             -EEXISTS if name already exists
 *             -ENOTDIR if dir_ino is not a directory inode
 */
static int _link(
    const char * name, uint32_t ino, uint32_t dir_ino, struct context * ctx)
{
    fprintf(stderr, "_link(name=%s, ino=%u, dir_ino=%u)\n",
        name, ino, dir_ino);
    assert(name);
    assert(ino);
    assert(dir_ino);
    assert(ctx);

    // TODO:

    // validate name length
    if (strlen(name) >= FSX492_FILENAMESZ) {
        fprintf(stderr, "link error: name too long\n");
        return -EINVAL;
    }

    // validate directory inode
    if (validate_inode(dir_ino, ctx) < 0) {
        fprintf(stderr, "link error: directory inode %u does not exist\n", dir_ino);
        return -ENOENT;
    }
    struct fsx492_inode * dir_inode = &ctx->inodes[dir_ino];
    if (!S_ISDIR(dir_inode->mode)) {
        fprintf(stderr, "link error: inode %u is not a directory\n", dir_ino);
        return -ENOTDIR;
    }
    //validate ino
    if (validate_inode(ino, ctx) < 0) {
        fprintf(stderr, "link error: target inode %u does not exist\n", ino);
        return -ENOENT;
    }
    //make sure name dne in dir already
    uint32_t dummy;
    int ret = find_entry(name, dir_ino, &dummy, ctx);
    if (ret == 0) {
        fprintf(stderr, "link error: name %s already exists in directory inode %u\n", name, dir_ino);
        return -EEXIST;
    } else if (ret != -ENOENT) {//atp we know dir exists and name dne, so any error other than -ENOENT is a disk error
        fprintf(stderr, "link error: failed to search for name %s in directory inode %u\n", name, dir_ino);
        return ret;
    }
    // load directory entries from disk - rn we know dir is valid and name is valid
    struct fsx492_dirent entries[FSX492_DIRENTRIES_PER_BLK];//will hold 1 block of dir entries as we search for free entry
    memset(entries, 0, sizeof(entries));
    for (int i=0; i<FSX492_N_DIRECT; i++) {
        uint32_t blkno = dir_inode->direct_blks[i];
        fprintf(stderr, "_link: checking slot %d, blkno=%u\n", i, blkno);
        if (blkno == 0 || validate_block(blkno, ctx) < 0) {//also checks if its a superblock cuz superblocks are invalid blocks for data
            //block is not allocated so we can use it for new entry
            fprintf(stderr, "_link: block %u is invalid, allocating new block\n", blkno);
            int ret = alloc_blk(&blkno, ctx);
            fprintf(stderr, "_link: alloc_blk returned %d, new blkno=%u\n", ret, blkno); //another debug print
            if (ret < 0) {
                fprintf(stderr, "link error: failed to allocate block for directory entries\n");
                return ret;
            }
            //get here = allocated succesfully, update dir inode and write back
            dir_inode->direct_blks[i] = blkno;
            dir_inode->blocks++;
            dirty_inode(dir_inode->ino, ctx);
            fprintf(stderr, "_link: set direct_blks[%d] = %u\n", i, blkno);
        }
    
        //a block is allocated so find a free directory entry (allocate new blocks as needed)
        fprintf(stderr, "_link: reading block %u\n", blkno);
        if (read_blks(blkno, 1, (void *)entries) < 0) {
            fprintf(stderr, "link error: failed to read directory entries from disk\n");
            return -EIO;
        }
        //otherwise we have a block of entries to search for a free one
        for (int j = 0; j < FSX492_DIRENTRIES_PER_BLK; j++) {
            if (!entries[j].valid) {//aka the entry is free - i hope lol
                //add info to the entry
                entries[j].valid = 1; entries[j].ino = ino;
                strncpy(entries[j].name, name, FSX492_FILENAMESZ-1);
                entries[j].name[FSX492_FILENAMESZ-1] = '\0';//gotta null term
                // write back modified entry to disk
                fprintf(stderr, "_link: writing to block %u, disk size %u\n", blkno, ctx->n_blocks);//debug line
                if (write_blks(blkno, 1, (void *)entries) < 0) {
                    fprintf(stderr, "link error: failed to write directory entries to disk\n");
                    return -EIO;
                }

                // modify directory inode
                dir_inode->size += sizeof(struct fsx492_dirent);
                dirty_inode(dir_ino, ctx);//each mod = dirty inode
                // modify entry inode
                struct fsx492_inode * entry_inode = &ctx->inodes[ino];
                entry_inode->nlink++;//cuz we added a new one!
                dirty_inode(ino, ctx);//since we j modded nlink, we gonna need a writeback later
                return 0;//success!
            }
        }
    }

    return -ENOSPC;//only occurs if all blocks were full of valid entries
}


/**
 * @brief      unlink an existing inode from the directory
 *             deletes frees the inode if inode has no more links
 *
 * @param[in]  name     The name to unlink
 * @param[in]  dir_ino  The directory inode number
 * @param      ctx      The file system context
 *
 * @return     0        on success
 *             -EIO     on disk error
 *             -ENOENT  if inode not in directory
 */
static int _unlink(
    const char * name, uint32_t dir_ino, struct context * ctx)
{
    assert(name);
    assert(dir_ino);
    assert(ctx);
    
    // TODO:

    // load entries from disk and search for the entry
    struct fsx492_dirent entries[FSX492_DIRENTRIES_PER_BLK];
    memset(entries, 0, sizeof(entries));
    struct fsx492_inode * dir_inode = &ctx->inodes[dir_ino];
    for (int i=0; i<FSX492_N_DIRECT; i++) {//go over all direct blocks of dir inode
        uint32_t blkno = dir_inode->direct_blks[i];
        if (blkno == 0 || validate_block(blkno, ctx) < 0) {continue;}//block aint valid/allocated so skip
        if (read_blks(blkno, 1, (void *)entries) < 0) {//try to read block of entries into memory
            fprintf(stderr, "unlink error: failed to read directory entries from disk\n");
            return -EIO;
        }
        //getting here means block is valid/allocated so search it
        for (int j=0; j<FSX492_DIRENTRIES_PER_BLK; j++) {
            if (entries[j].valid && strcmp(entries[j].name, name) == 0) {
                //we found it - so unlink it!
                // invalidate the entry
                uint32_t ino = entries[j].ino; entries[j].valid = 0;

                // write back modified entries
                if (write_blks(blkno, 1, (void *)entries) < 0) {
                    fprintf(stderr, "unlink error: failed to write directory entries to disk\n");
                    return -EIO;
                }
                // change directory file size after writeback succeeds
                dir_inode->size -= sizeof(struct fsx492_dirent);
                dirty_inode(dir_ino, ctx);//dirty inode after mod

                // decrement inode nlink
                struct fsx492_inode * entry_inode = &ctx->inodes[ino];
                entry_inode->nlink--;
                dirty_inode(ino, ctx);//dirty inode after mod

                // delete inode if necessary
                if (entry_inode->nlink == 0) {
                    if (_truncate(ino, 0, ctx) < 0) {
                        fprintf(stderr, "unlink error: failed to truncate inode %u\n", ino);
                        return -EIO;
                    }
                    free_inode(ino, ctx);
                }

                fprintf(stderr, "Successfully unlinked entry %s from directory inode %u\n", name, dir_ino);
                return 0;//success!
            }
        }
    }
    fprintf(stderr, "unlink error: entry %s not found in directory inode %u\n", name, dir_ino);
    return -ENOENT;
}


/*****************************************************************************
 * CALLBACK IMPLEMENTATIONS
 *****************************************************************************/

/**
 * fuse operations vector (callback bindings)
 * 
 * we only implement a subset of operations
 */
struct fuse_operations fsx492_ops = {
    .init       = fsx492_init,
    .destroy    = fsx492_destroy,
    .getattr    = fsx492_getattr,
    .opendir    = fsx492_opendir,
    .readdir    = fsx492_readdir,
    .releasedir = fsx492_releasedir,
    .mknod      = fsx492_mknod,
    .mkdir      = fsx492_mkdir,
    .unlink     = fsx492_unlink,
    .rmdir      = fsx492_rmdir,
    .rename     = fsx492_rename,
    .link       = fsx492_link,
    .chmod      = fsx492_chmod,
    .utimens    = fsx492_utimens,
    .truncate   = fsx492_truncate,
    .open       = fsx492_open,
    .read       = fsx492_read,
    .write      = fsx492_write,
    .release    = fsx492_release,
    .statfs     = fsx492_statfs,

    ////
    .symlink = fsx492_symlink,
    .readlink = fsx492_readlink

};


/**
 * @brief      initialization function called by fuse
 *             initializes `struct context` from disk
 *
 * @param      conn  The connection (unused)
 * @param      cfg   The configuration (unused)
 *
 * @return     pointer to file system context
 */
void * fsx492_init(struct fuse_conn_info * conn, struct fuse_config * cfg)
{
    fprintf(stdout, "fsx492_init\n");
    assert(disk);
    assert(disk->ops);
    struct fsx492_superblk sb;
    int result = 0;

    if (_ctx->initialized) {
        // don't need to reinitialize
        fprintf(stderr, "context already initialized\n");
        return (void *)_ctx;
    }

    // read superblock and initialize context

    if (disk->ops->read(disk, 0, 1, (void *)&sb) < 0) {
        fprintf(stderr, "failed to read superblock\n");
        exit(1);
    }

    // calculate region sizes
    _ctx->inode_map_base = 1;
    _ctx->block_map_base = _ctx->inode_map_base + sb.inode_map_sz;
    _ctx->inodes_base = _ctx->block_map_base + sb.block_map_sz;
    _ctx->n_inodes = sb.inode_region_sz * FSX492_INODES_PER_BLK;

    _ctx->n_metablks = 
        1 + sb.inode_map_sz + sb.block_map_sz + sb.inode_region_sz;

    if (!(_ctx->metadata = malloc(_ctx->n_metablks * FSX492_BLKSZ))) {
        fprintf(stderr, "failed to allocate space for metadata blocks\n");
        exit(1);
    }

    result = read_blks(0, _ctx->n_metablks, _ctx->metadata);
    if (result < 0) {
        fprintf(stderr, "failed to read metadata blocks\n");
        exit(1);
    }

    _ctx->inode_map = (fd_set *)((uint8_t *)_ctx->metadata + FSX492_BLKSZ);
    _ctx->block_map = (fd_set *)((uint8_t *)_ctx->metadata
        + (1 + sb.inode_map_sz) * FSX492_BLKSZ);
    _ctx->inodes = (struct fsx492_inode *)((uint8_t *)_ctx->metadata
        + (1 + sb.inode_map_sz + sb.block_map_sz) * FSX492_BLKSZ);

    // allocate bitmap for tracking dirty metadata
    size_t dirty_map_sz = _ctx->n_metablks / sizeof(uint32_t) + 1;
    if (!(_ctx->dirty_map = calloc(dirty_map_sz, sizeof(uint32_t)))) {
        fprintf(stderr, "failed to allocate dirty_map\n");
        exit(1);
    }

    _ctx->n_blocks = sb.total_blocks;
    _ctx->root_inode = sb.root_inode;
    _ctx->data_base = _ctx->inodes_base + sb.inode_region_sz;
    _ctx->initialized = true;

    return (void *)_ctx;
}


/**
 * @brief      cleanup file system
 *
 * @param      private_data  The pointer to file system context
 * 
 * @note       free everything allocated during initialization
 *             and close the disk.
 */
void fsx492_destroy(void * private_data)
{
    fprintf(stdout, "fsx492_destroy\n");
    assert(disk);
    assert(disk->ops);
    assert(private_data);
    struct context * ctx = (struct context *)private_data;

    fprintf(stdout, "writing dirty metadata to image...\n");
    writeback_metadata(ctx);

    fprintf(stdout, "freeing context...\n");
    if (ctx->metadata) free(ctx->metadata);
    if (ctx->dirty_map) free(ctx->dirty_map);

    if (disk->private) {
        fprintf(stdout, "closing disk...\n");
        disk->ops->close(disk);
    }

    fprintf(stdout, "fsx492 destroyed\n");
}


/**
 * @brief      get file or directory attributes
 *
 * @param[in]  path     The path
 * @param      statbuf  The file stat buffer to populate
 *
 * @return     0        on success
 *             -EIO     on failure to read disk
 *             -ENOENT  if any component of path does not exist
 *             -ENOTDIR if any intermediate component of path isn't a directory
 * 
 * @note       relevent documentation from <fuse.h> included below
 *             the `struct stat` field should be populated with information
 *             about the file or directory at the given path
 * 
 * ~~~~~~~
 * 
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored. The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given. In that case it is passed to userspace,
 * but libfuse and the kernel will still assign a different
 * inode for internal use (called the "nodeid").
 *
 * `fi` will always be NULL if the file is not currently open, but
 * may also be NULL if the file is open.
 */
int fsx492_getattr(
    const char * path, struct stat * statbuf, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_getattr: %s\n", path);
    assert(path);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    // TODO:
    // lookup inode (or skip lookup if handle already open in fi)
    uint32_t ino = 0; int ret = 0;
    if (fi && fi->fh) { //file handle exists from previous open
        ino = ((struct fh *)fi->fh)->ino;
    }
    else {
        ret = LP(path, &ino, NULL);
        if (ret < 0) {
            fprintf(stderr, "fsx492_getattr: LP failed with %d\n", ret);
            return ret;
        }
    }
        //also make sure inode exists and is valid
    if (validate_inode(ino, ctx) < 0) {
        fprintf(stderr, "fsx492_getattr: inode %u does not exist\n", ino);
        return -ENOENT;
    }

    // copy stat info to statbuf
    copy_stat(&ctx->inodes[ino], statbuf);

    return 0;
}


/**
 * @brief      create a new file with specified mode bits
 *             this may be used to create regular files and special files,
 *             however, we will not be implementing special files.
 *
 * @param[in]  path  The path
 * @param[in]  mode  The mode
 * @param[in]  dev   The device specification (unused)
 *
 * @return     0        on success
 *             -EIO     on failure to read disk
 *             -ENOENT  if any component of path does not exist
 *             -ENOTDIR if any intermediate component of path isn't a directory
 *             -EEXIST  if file already exists
 *             -EINVAL  on any of the following:
 *                       - create special file
 *                       - overwrite root
 *                       - file name too long
 *             -ENOSPC  if free inode unavailable or directory is full
 * 
 * @note       relevant documentation from <fuse.h> is included below
 * 
 * ~~~~~~~
 * 
 * This is called for creation of all non-directory, non-symlink
 * nodes.  If the filesystem defines a create() method, then for
 * regular files that will be called instead.
 */
int fsx492_mknod(const char * path, mode_t mode, dev_t dev)
{
    fprintf(stdout, "fsx492_mknod: %s\n", path);
    if (!S_ISREG(mode)) {
        // attempting to create non-regular file
        return -EINVAL;
    }

    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    int ret = 0;
    uint32_t target_ino = 0, parent_ino = 0;

    switch (ret = LP(path, &target_ino, &parent_ino)) {
    case 0:         // the path was found
        return -EEXIST;
    case -EIO:      // disk error
    case -ENOTDIR:  // bad path
    case -EINVAL:   // bad path
        return ret;
    case -ENOENT:
        if (!target_ino) {
            // bad path
            return ret;
        }
        break;
    default:
        assert(0); // unreachable
    }

    // parent_ino should be correct now
    assert(parent_ino);
    
    // create a new inode
    uint32_t ino = 0;
    if ((ret = alloc_inode(&ino, ctx)) < 0) {
        fprintf(stderr, "fsx492_mknod: failed to allocate inode\n");
        return ret;
    }
    assert(ino);

    // initialize inode fields
    struct fsx492_inode * inode = &ctx->inodes[ino];
    inode->ino = ino;
    inode->mode = mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlink = 0;
    inode->blocks = 0;
    inode->ctime = inode->mtime = inode->atime = time(NULL);
    for (int i = 0; i < FSX492_N_DIRECT; i++) {
        inode->direct_blks[0] = 0;
    }
    inode->indir1_blks = 0;
    inode->indir2_blks = 0;

    // mark inodes block as dirty
    dirty_inode(ino, ctx);

    fprintf(stderr, "fsx492_mknod: created file %d: %s\n", ino, path);

    // link inode to directory
    if ((ret = _link(basename(path), ino, parent_ino, ctx)) < 0) {
        fprintf(stderr, "fsx492_mknod: failed to link inode\n");
        free_inode(ino, ctx);
    }
    return ret;
}


/**
 * @brief      open a file by path
 *
 * @param[in]  path  The path
 * @param      fi    The fuse file info
 *
 * @return     0        on success
 *             -EIO     on failure to read disk
 *             -ENOENT  if any component of path does not exist
 *             -ENOTDIR if any intermediate component of path isn't a directory
 *             -ENOSPC  if failed to allocate file handle
 * 
 * @note       the `fuse_file_info` struct has a `fh` field for storing
 *             file handles. this is much more efficient than performing
 *             a path lookup on every read/write access.
 *             see open(2) for flags
 *             https://man7.org/linux/man-pages/man2/open.2.html
 * 
 * @note       relevent documentation from <fuse.h> included below
 * 
 * ~~~~~~~
 *
 * Filesystem may store an arbitrary file handle (pointer,
 * index, etc) in fi->fh, and use this in other all other file
 * operations (read, write, flush, release, fsync).
 *
 * Filesystem may also implement stateless file I/O and not store
 * anything in fi->fh.
 * 
 * ~~~~~~~
 *
 */
int fsx492_open(const char * path, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_open: %s\n", path);
    assert(fi);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    // TODO:
    if (fi->fh) {
        // file handle already exists from previous open
        return 0;
    }

    // lookup path and validate inode
    uint32_t ino = 0;
    int ret = LP(path, &ino, NULL);
    if (ret < 0) {//some failure with lookup
        fprintf(stderr, "fsx492_open: LP failed with %d\n", ret);
        return ret;
    }
    if (validate_inode(ino, ctx) < 0) {
        fprintf(stderr, "fsx492_open: inode %u does not exist\n", ino);
        return -ENOENT;
    }//tho idt it ever gets this far - i think look_up path would fail in this case

    // (option: perform permissions checking)

    // create the file handle
    struct fh * handle = malloc(sizeof(struct fh));
    if (!handle) {
        fprintf(stderr, "fsx492_open: failed to allocate file handle\n");
        return -ENOSPC;
    }

    // store file handle in fi->fh
    handle->ino = ino; handle->flags= fi->flags;
    fi->fh = (uint64_t) handle;

    return 0;
}


/**
 * @brief      attempt to read `size` bytes from file into `buf`
 *             read operation commences at file offset `offset`
 *
 * @param[in]  path     The path
 * @param      buf      The destination buffer
 * @param[in]  n        The number of bytes to read
 * @param[in]  offset   The offset to read from
 * @param      fi       The fuse file info
 *
 * @return     exact number of bytes read
 *             -EISDIR  file is a directory
 *             -EBADF   on bad file handle
 *             -EIO     on disk error
 * 
 * @note       relevent documentation from <fuse.h> included below
 * 
 * ~~~~~~~
 * 
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 */
int fsx492_read(const char * path, char * buf, size_t size,
    off_t offset, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_read: %s (size=%lu, offset=%ld)\n", path, size, offset);
    assert(path);
    assert(buf);
    assert(fi);

    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    int ino = ((struct fh *)fi->fh)->ino;
    fprintf(stderr, "fsx492_read: reading inode %u\n", ino);

    if (validate_inode(ino, ctx) < 0) {
        return -EBADF;
    }

    struct fsx492_inode * inode = &ctx->inodes[ino];

    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (offset >= inode->size) return 0;

    if (offset + size >= inode->size) {
        size = inode->size - offset;
    }

    char tmpbuf[FSX492_BLKSZ];
    size_t to_read = size;

    // read direct blocks
    const size_t direct_sz = FSX492_N_DIRECT * FSX492_BLKSZ;
    if (to_read > 0 && offset < direct_sz) {
        // calculate offset of starting direct block
        const size_t start_blk = offset / FSX492_BLKSZ;
        for (size_t i = start_blk; to_read > 0 && i < FSX492_N_DIRECT; i++) {
            if (read_blks(inode->direct_blks[i], 1, (void *)tmpbuf) < 0) {
                return -EIO;
            }
            // calculate block offset and length to read
            size_t blk_offset = (offset % FSX492_BLKSZ);
            size_t blk_rlen = (to_read > FSX492_BLKSZ) ? (
                FSX492_BLKSZ - blk_offset
            ) : (
                to_read
            );
            
            // copy data to destination
            memcpy(buf, tmpbuf + blk_offset, blk_rlen);

            // update state variables
            to_read -= blk_rlen;
            offset += blk_rlen;
            buf += blk_rlen;
        }
    }

    // read indir1 blocks
    const size_t indir1_sz = (
        FSX492_N_DIRECT + FSX492_PTRS_PER_BLK
    ) * FSX492_BLKSZ;
    if (to_read > 0 && offset < indir1_sz) {
        // read indir1 block
        uint32_t blks[FSX492_PTRS_PER_BLK];
        if (read_blks(inode->indir1_blks, 1, (void *)blks) < 0) {
            return -EIO;
        }

        // compute starting data block in indir1 block
        const size_t start_blk = offset / FSX492_BLKSZ - FSX492_N_DIRECT;
        for (size_t i = start_blk; to_read > 0 && i < FSX492_PTRS_PER_BLK; i++) {
            if (read_blks(blks[i], 1, (void *)tmpbuf) < 0) {
                return -EIO;
            }
            size_t blk_offset = (offset % FSX492_BLKSZ);
            size_t blk_rlen = (to_read > FSX492_BLKSZ) ? (
                FSX492_BLKSZ - blk_offset
            ) : (
                to_read
            );
            memcpy(buf, tmpbuf + blk_offset, blk_rlen);
            to_read -= blk_rlen;
            offset += blk_rlen;
            buf += blk_rlen;
        }
    }

    // read indir2 blocks
    const size_t indir2_sz = (
        FSX492_N_DIRECT
        + FSX492_PTRS_PER_BLK
        + FSX492_PTRS_PER_BLK * FSX492_PTRS_PER_BLK
    ) * FSX492_BLKSZ;
    if (to_read > 0 && offset < indir2_sz) {
        // read indir2 block
        uint32_t blks2[FSX492_PTRS_PER_BLK];
        uint32_t blks1[FSX492_PTRS_PER_BLK];
        if (read_blks(inode->indir2_blks, 1, (void *)blks2) < 0) {
            return -EIO;
        }

        // compute starting indir1 block in indir2 block
        const size_t start_i = (
            offset / FSX492_BLKSZ - FSX492_N_DIRECT - FSX492_PTRS_PER_BLK
        ) / FSX492_PTRS_PER_BLK;
        for (size_t i = start_i; to_read > 0 && i < FSX492_PTRS_PER_BLK; i++) {
            // read indir1 block
            if (read_blks(blks2[i], 1, (void *)blks1) < 0) {
                return -EIO;
            }

            // compute starting data block in indir1 block
            const size_t start_j = (
                offset / FSX492_BLKSZ - FSX492_N_DIRECT - FSX492_PTRS_PER_BLK
            ) % FSX492_PTRS_PER_BLK;
            for (size_t j = start_j; to_read > 0 && j < FSX492_PTRS_PER_BLK; j++) {
                if (read_blks(blks1[j], 1, (void *)tmpbuf) < 0) {
                    return -EIO;
                }
                size_t blk_offset = (offset % FSX492_BLKSZ);
                size_t blk_rlen = (to_read > FSX492_BLKSZ) ? (
                    FSX492_BLKSZ - blk_offset
                ) : (
                    to_read
                );
                memcpy(buf, tmpbuf + blk_offset, blk_rlen);
                to_read -= blk_rlen;
                offset += blk_rlen;
                buf += blk_rlen;
            }
        }
    }

    return (int)(size - to_read);
}


/**
 * @brief      attempt to write `size` bytes from `buf` into file,
 *             starting from file offset `offset`
 *
 * @param[in]  path    The path
 * @param[in]  buf     The buffer
 * @param[in]  size    The size
 * @param[in]  offset  The offset
 * @param      fi      The fuse file info
 *
 * @return     exact number of bytes requested
 *             -EIO    on disk error
 *             -EBADF  on bad file handle
 *             -EINVAL if offset is larger than file size
 *             
 * @note       write should allocate more data blocks if needed
 *             see write(2) manpage
 *             https://man7.org/linux/man-pages/man2/write.2.html
 *             
 * @note       relevent documenation from <fuse.h> included below
 * 
 * ~~~~~~~
 * 
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */


int editBlk(uint32_t blkID, int start, char* buf, int bufOffset){
    if (start){
        // if start != 0, will have to split the block in 2 parts, and write to part of it
        char blkBuf[FSX492_BLKSZ];
        if (read_blks(blkID, 1, blkBuf) < 0) return -EIO;
        memcpy(blkBuf + start, buf + bufOffset, FSX492_BLKSZ-start);
        if (write_blks(blkID, 1, blkBuf) < 0) return -EIO;              
    }else{
        // if start does equal 0, will write to it in one shot
        write_blks(blkID, 1, buf+bufOffset);
    }
    return 0;
}
int dealloc(){

};


#define cp2ip(X) ((uint32_t*)(void*)(X))
int fsx492_write(const char * path, const char * buf, size_t size,
    off_t offset, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_write: %s (size=%lu, offset=%ld)\n", path, size, offset);
    assert(path);
    assert(buf);
    assert(fi);

    struct context * ctx = (struct context *)fuse_get_context()->private_data;


    // validate file handle and other arguments
    struct fh * fhandle = (struct fh *)fi->fh;
    uint32_t ino = fhandle->ino;
    fprintf(stderr, "fsx492_write: writing inode %u\n", ino);
    if (validate_inode(ino, ctx) < 0) {
        return -EBADF;
    }

    struct fsx492_inode * inode = &ctx->inodes[ino];

    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (offset > inode->size) return -EINVAL;
    if (fhandle->flags & O_APPEND) offset = inode->size;

    int errcatch;
    // write to direct blocks if needed (allocate space as needed)
    long writeptr = offset;
    long bitesWritten = 0;
    long target = offset + size;
    while (writeptr < FSX492_BLKSZ*FSX492_N_DIRECT && writeptr < target) {
        long inblock = writeptr /  (long)(FSX492_BLKSZ);
        long start = writeptr - inblock*FSX492_BLKSZ;
        if (inblock >= inode->blocks) {
            if (errcatch = alloc_blk(&inode->direct_blks[inblock], ctx)) return errcatch;
            inode->blocks+=1;
        }
        if (errcatch = editBlk(inode->direct_blks[inblock],start,buf,bitesWritten)) return errcatch;

        bitesWritten += FSX492_BLKSZ-start;
        inblock += 1;
        writeptr = FSX492_BLKSZ*inblock;
    }
    
    // write to indir1 blocks if needed (allocate space as needed)
    long indirBase = FSX492_N_DIRECT; //in BLOCKS
    long indirEnd = indirBase + FSX492_BLKSZ/sizeof(uint32_t); //in BLOCKS
    char indirBlkData[FSX492_BLKSZ];
    if (writeptr<target && inode->indir1_blks) if (errcatch = read_blks(inode->indir1_blks, 1, indirBlkData)) return errcatch;

    char modifiedInDir = 0;
    while (writeptr < target) {
        long inblock = writeptr /  (long)(FSX492_BLKSZ);
        if (inblock >= indirEnd) break;
        long start = writeptr - inblock*FSX492_BLKSZ;

        //ALLOCATIONS
        if (!(inode->indir1_blks )) {
            if (errcatch = alloc_blk(&inode->indir1_blks, ctx)) return errcatch;
            if (errcatch = read_blks(inode->indir1_blks, 1, indirBlkData)) return errcatch;
            modifiedInDir = true;
        }
        if (inblock >= inode->blocks) {
            if (errcatch = alloc_blk(cp2ip(indirBlkData) + (inblock - indirBase), ctx)) return errcatch;
            inode->blocks+=1;
            modifiedInDir = true;
        }

        //UPDATE
        if (errcatch = editBlk(cp2ip(indirBlkData)[inblock - indirBase],start,buf,bitesWritten)) return errcatch;        
        bitesWritten += FSX492_BLKSZ-start;
        inblock += 1;
        writeptr = FSX492_BLKSZ*inblock;
    }

    //update the indirect block with any changes made to indirBlkData
    if (modifiedInDir) if (errcatch = editBlk(inode->indir1_blks,0,indirBlkData,0)) return errcatch;


    // write to indir2 blocks if needed (allocate space as needed)
    {//extra set of brackets for scoping reasons
    long indir2Base = indirEnd //in BLOCKS
    long indir2End = indir2Base + (FSX492_BLKSZ/sizeof(uint32_t)) * (FSX492_BLKSZ/sizeof(uint32_t)) //in BLOCKS
    char indir2BlkData[FSX492_BLKSZ];
    if (writeptr<target && inode->indir2_blks) if (errcatch = read_blks(inode->indir2_blks, 1, indir2BlkData)) return errcatch;

    long indirBase = indirEnd; //in BLOCKS
    long indirEnd = indirBase + FSX492_BLKSZ/sizeof(uint32_t); //in BLOCKS
    char indirBlkData[FSX492_BLKSZ];
    if (writeptr<target && cp2ip(indir2BlkData)[0]) if (errcatch = read_blks(cp2ip(indir2BlkData)[0], 1, indirBlkData)) return errcatch;
    char modifiedInDir2 = false;
    char modifiedInDir = false;
    int indir2idx = 0;
    while (writeptr < target){
        long inblock = writeptr /  (long)(FSX492_BLKSZ);
        if (inblock >= indir2End) return -EINVAL;

        //ALLOCS
        if (!inode->indir2_blks){
            if (errcatch = alloc_blk(&inode->indir2_blks, ctx)) return errcatch;
            if (errcatch = read_blks(inode->indir2_blks, 1, indir2BlkData)) return errcatch;
            modifiedInDir2 = true;
        }
        if (inblock >= indirEnd) {
            //move to next indir block in the indir2 block
            if (modifiedInDir) if (errcatch = editBlk(cp2ip(indir2BlkData)[indir2idx],0,indirBlkData,0)) return errcatch;
            modifiedInDir = false;
            indirBase = indirEnd;
            indirEnd = indirBase + FSX492_BLKSZ/sizeof(uint32_t);
            indir2idx++;
            if (writeptr<target && cp2ip(indir2BlkData)[indir2idx]) {
                if (errcatch = read_blks(cp2ip(indir2BlkData)[indir2idx], 1, indirBlkData)) return errcatch;
            }else {
                if (errcatch = alloc_blk(cp2ip(indir2BlkData) + indir2idx, ctx)) return errcatch;
            }
        }
        if (inblock >= inode->blocks) {
            if (errcatch = alloc_blk(cp2ip(indirBlkData) + (inblock - indirBase), ctx)) return errcatch;
            inode->blocks+=1;
            modifiedInDir = true;
        }

        //UPDATE
        long start = writeptr - inblock*FSX492_BLKSZ;
        if (errcatch = editBlk(cp2ip(indirBlkData)[inblock - indirBase],start,buf,bitesWritten)) return errcatch;        
        bitesWritten += FSX492_BLKSZ-start;
        inblock += 1;
        writeptr = FSX492_BLKSZ*inblock;   
    }
    if (modifiedInDir2) if (errcatch = editBlk(inode->indir2_blks,0,indir2BlkData,0)) return errcatch;
    }
    // update inode and mark dirty
    inode->mtime = time(NULL);
    _truncate(inode->ino, size, ctx);    
    dirty_inode(ino, ctx);
    return  bitesWritten;
}


/**
 * @brief      release an open file
 *
 * @param[in]  path  The path
 * @param      fi    The fuse file info
 *
 * @return     0        on success
 *             -EIO     on disk error
 *             -ENOENT  path does not exist
 *             -ENOTDIR path component is not directory
 *             -EINVAL  path component is invalid
 *             -EISDIR  path is a directory
 *
 * @note       this call should write back metadata
 *             relevant documentation from <fuse.h> is included below
 *             
 * ~~~~~~~
 * 
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file handle.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 */
int fsx492_release(const char * path, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_release: %s\n", path);
    assert(path);

    // TODO:

    // release resources from opened file (e.g. file handle)
    if (!fi || !fi->fh) {
        return 0; // nothing to release
    }
    struct fh * handle = (struct fh *)fi->fh;
    free(handle);
    fi->fh = 0;

    // write back metadata
    writeback_metadata((struct context *)fuse_get_context()->private_data);

    return 0;
}


/**
 * @brief      create directory
 *
 * @param[in]  path  The path
 * @param[in]  mode  The mode
 *
 * @return     0        on success
 *             -EIO     on failure to read disk
 *             -ENOENT  if any component of path does not exist
 *             -ENOTDIR if any intermediate component of path isn't a directory
 *             -EEXIST  if file or directory already exists
 *             -EINVAL  on any of the following:
 *                       - overwrite root
 *                       - file name too long
 *             -ENOSPC  if free inode unavailable or directory is full
 * 
 * 
 * @note       relevant documentation from <fuse.h> included below
 * 
 * ~~~~~~~
 * 
 * Note that the mode argument may not have the type specification
 * bits set, i.e. S_ISDIR(mode) can be false.  To obtain the
 * correct directory type bits use  mode|S_IFDIR
 */
int fsx492_mkdir(const char * path, mode_t mode)
{
    fprintf(stdout, "fsx492_mkdir: %s\n", path);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    // TODO:
    //check name length
    if (strlen(basename(path)) >= FSX492_FILENAMESZ) {
        fprintf(stderr, "fsx492_mkdir: file name too long\n");
        return -EINVAL;
    }
    //check if directory already exists at path
    uint32_t ino = 0; uint32_t parent_ino = 0;
    // lookup parent directory path (see docs for `LP`)
    int ret = LP(path, &ino, &parent_ino);
    //check if dir alr exists at path
    if (ret == 0) {
        fprintf(stderr, "fsx492_mkdir: directory already exists at path\n");
        return -EEXIST;
    }
    //otherwise it does not. check if there was another failure
    if (ret != -ENOENT) {//not found error is expected since we r creating new dir so ignore
        fprintf(stderr, "fsx492_mkdir: LP failed with %d\n", ret);
        return ret;
    }
    //check if parent_ino dne which we do not want
    if (!parent_ino) {
        fprintf(stderr, "fsx492_mkdir: parent directory does not exist\n");
        return -ENOENT;
    }
    //also check that its a dir
    if (!S_ISDIR(ctx->inodes[parent_ino].mode)) {
        fprintf(stderr, "fsx492_mkdir: parent path component is not a directory\n");
        return -ENOTDIR;
    }

    // create a new directory inode
    uint32_t dir_ino = 0;
    if (alloc_inode(&dir_ino, ctx) < 0) {
        fprintf(stderr, "fsx492_mkdir: failed to allocate inode\n");
        return -ENOSPC;
    }

    // allocate space for directory entries
    struct fsx492_inode * dir_inode = &ctx->inodes[dir_ino];
    dir_inode->ino = dir_ino;
    dir_inode->mode = mode | S_IFDIR;
    dir_inode->uid = getuid();
    dir_inode->gid = getgid();
    dir_inode->size = 0;
    dir_inode->nlink = 2; // `.` and `..` entries
    dir_inode->blocks = 0;
    dir_inode->ctime = dir_inode->mtime = dir_inode->atime = time(NULL);
    for (int i=0; i<FSX492_N_DIRECT; i++) {dir_inode->direct_blks[i] = 0;}
    dir_inode->indir1_blks = 0; dir_inode->indir2_blks = 0;

    // add `.` and `..` subdirectories
    //since dir is new i gotta alloc blocks for these entries
    uint32_t dir_blk = 0;
    if (alloc_blk(&dir_blk, ctx) < 0) {
        fprintf(stderr, "fsx492_mkdir: failed to allocate block for directory entries\n");
        free_inode(dir_ino, ctx);
        return -ENOSPC;
    }
    //block allocated, make space for 2 entries
    dir_inode->direct_blks[0] = dir_blk; dir_inode->blocks=1;
    dir_inode->size = 2 * sizeof(struct fsx492_dirent);
    struct fsx492_dirent entries[FSX492_DIRENTRIES_PER_BLK];
    memset(entries, 0, sizeof(entries));
    //create the 2 entries
    entries[0].valid = 1; entries[0].ino = dir_ino;
    strncpy(entries[0].name, ".", FSX492_FILENAMESZ-1);
    entries[0].name[FSX492_FILENAMESZ-1] = '\0';
    entries[1].valid = 1; entries[1].ino = parent_ino;
    strncpy(entries[1].name, "..", FSX492_FILENAMESZ-1);
    entries[1].name[FSX492_FILENAMESZ-1] = '\0';
    if (write_blks(dir_blk, 1, entries) < 0) {//write entries to disk
        fprintf(stderr, "fsx492_mkdir: failed to write directory entries\n");
        free_inode(dir_ino, ctx);
        return -EIO;
    }
    dirty_inode(dir_ino, ctx);

    // link new directory to parent directory
    if (_link(basename(path), dir_ino, parent_ino, ctx) < 0) {
        fprintf(stderr, "fsx492_mkdir: failed to link new directory to parent directory\n");
        //free block allocated for entries
        free_blk(dir_blk, ctx);
        free_inode(dir_ino, ctx);
        return -EIO;
    }
    //also inc parent dir's nlink for new subdir
    ctx->inodes[parent_ino].nlink++;
    dirty_inode(parent_ino, ctx);
    // mark dirty inodes for writeback
    return 0;
}


/**
 * @brief      open a directory by path
 *
 * @param[in]  path  The path
 * @param      fi    The fuse file info
 *
 * @return     0        on success
 *             -EIO     on failure to read disk
 *             -ENOENT  if any component of path does not exist
 *             -ENOTDIR if any intermediate component of path isn't a directory
 *                      or if the target path is not a directory
 *             -ENOSPC  failed to allocate file handle
 */
int fsx492_opendir(const char * path, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_opendir: %s\n", path);
    assert(fi);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    
    // TODO:

    // look up the directory inode
    uint32_t ino = 0; int ret = LP(path, &ino, NULL);
    if (ret < 0) {return ret;}//aka there was a failure
    if (!S_ISDIR(ctx->inodes[ino].mode)) {
        fprintf(stderr, "fsx492_opendir: target is not a directory\n");
        return -ENOTDIR;
    }//check if it is a dir

    //getting here means its a dir with valid path
    // create a new file handle
    struct fh * handle = malloc(sizeof(struct fh));
    if (!handle) {
        fprintf(stderr, "fsx492_opendir: failed to allocate file handle\n");
        return -ENOSPC;
    }
    handle->ino = ino; handle->flags = fi->flags;
    // (optional) perform permissions checking

    // update fi with file handle
    fi->fh = (uint64_t)handle;
    fprintf(stdout, "fsx492_opendir: opened directory %u: %s\n", ino, path);

    return 0;
}


/**
 * @brief      read directory contents
 *             for each entry in the directory, use the filler function
 *             to add directory entries to the buffer
 *
 * @param[in]  path       The path
 * @param      buf        The buffer to fill
 * @param[in]  filler     The filler function
 * @param[in]  offset     The dirent offset (ignored)
 * @param      fi         The fuse file info
 * @param[in]  flags      The fuse readdir flags
 *
 * @return     0 on success
 *             -EIO on disk error
 *             -EBADF on bad file handle
 * 
 * @note       relevant documentation from <fuse.h> included below
 *             we have chosen option 1 to implement
 * 
 * ~~~~~~~
 * 
 * The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.
 * 
 * ~~~~~~~
 * 
 * @note       this is the documentation for the filler function
 * 
 * ~~~~~~~
 * 
 * The *off* parameter can be any non-zero value that enables the
 * filesystem to identify the current point in the directory
 * stream. It does not need to be the actual physical position. A
 * value of zero is reserved to indicate that seeking in directories
 * is not supported.
 *
 * @param buf the buffer passed to the readdir() operation
 * @param name the file name of the directory entry
 * @param stbuf file attributes, can be NULL
 * @param off offset of the next entry or zero
 * @param flags fill flags
 * @return 1 if buffer is full, zero otherwise
 * 
 */
int fsx492_readdir(const char * path, void * buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{ 
    fprintf(stdout, "fsx492_readdir: %s\n", path);
    if (!(fi && fi->fh)) {
        // bad file handle
        fprintf(stderr, "fsx492_readdir: invalid file handle\n");
        return -EBADF;
    }

    uint32_t ino = ((struct fh *)fi->fh)->ino;
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    struct fsx492_inode * dir_inode = &(ctx->inodes[ino]);

    if (!S_ISDIR(dir_inode->mode)) {
        fprintf(stderr, "fsx492_readdir: target is not a directory\n");
        return -ENOTDIR;
    }

    struct stat statbuf;
    struct fsx492_dirent entries[FSX492_DIRENTRIES_PER_BLK];
    memset(entries, 0, sizeof(entries));
    
    for (int i = 0; i < FSX492_N_DIRECT; i++) {
        if (dir_inode->direct_blks[i] == 0) {continue;} //skip unalloc blocks
        // load the direct block of entries
        if (read_blks(dir_inode->direct_blks[i], 1, (void *)entries) < 0) {
            fprintf(stderr, "fsx492_readdir: failed to read directory block\n");
            return -EIO;
        }

        // copy entries to buffer
        for (int i = 0; i < FSX492_DIRENTRIES_PER_BLK; i++) {
            if (!entries[i].valid) { continue; }

            copy_stat(&ctx->inodes[entries[i].ino], &statbuf);
            filler(buf, entries[i].name, &statbuf, 0, 0);
        }
    }
    // limit directory entries to direct blocks, don't visit indirect
    fprintf(stdout, "fsx492_readdir: finished reading directory %u: %s\n", ino, path);

    return 0;
}


/**
 * @brief      release directory
 *             if allocations occured in opendir, free them here
 *
 * @param[in]  path  The path
 * @param      fi    The fuse file info
 *
 * @return     0 on success
 *             -EIO on disk error
 * 
 * @note       relevant documentation from <fuse.h> included below
 * 
 * ~~~~~~~
 * 
 * If the directory has been removed after the call to opendir, the
 * path parameter will be NULL.
 */
int fsx492_releasedir(const char * path, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_releasedir: %s\n", path);
    assert(fi);
    
    // TODO:

    // free allocated resources (file handle)
    if (fi && fi->fh) {
        free((struct fh *)fi->fh);
        fi->fh = 0;
    }

    // write back dirty metadata
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    writeback_metadata(ctx);
    fprintf(stdout, "fsx492_releasedir: released directory %s\n", path);

    return 0;
}


/**
 * @brief      create a hard link to an existing file
 *
 * @param[in]  oldpath  The old file path
 * @param[in]  newpath  The new file path
 *
 * @return     0        on success
 *             -EMLINK  if oldpath has too many links
 *             -ENOENT  if the oldpath file does not exist
 *             -ENOTDIR if any component of old or new path is not a directory
 *             -ENOSPC  if directory full
 *             -EINVAL  if name too long
 */
int fsx492_link(const char * oldpath, const char * newpath)
{
    fprintf(stdout, "fsx492_link: %s -> %s\n", newpath, oldpath);
    assert(oldpath);
    assert(newpath);

    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    int errcatch;

    //source must exist and must not be a directory
    uint32_t old_ino = 0;
    if ((errcatch = LP(oldpath, &old_ino, NULL)) < 0) return errcatch;
    if (S_ISDIR(ctx->inodes[old_ino].mode)) return -EISDIR;

    // destination parent must exist, target must not
    uint32_t new_ino = 0, new_parent_ino = 0;
    if ((errcatch = LP(newpath, &new_ino, &new_parent_ino)) <=0) {
        if (errcatch == 0) return -EEXIST;     // "success" means failure here- should not be found
        if (errcatch != -ENOENT) return errcatch;     //ENOENT is *wanted* here
    };

    if (ctx->inodes[old_ino].nlink == UINT16_MAX) return -EMLINK;    // check for overflow
    return _link(basename(newpath), old_ino, new_parent_ino, ctx);
}


/**
 * @brief      remove a file
 *
 * @param[in]  path  The path
 *
 * @return     0        on success
 *             -EIO     on disk error
 *             -ENOENT  if path does not exist
 *             -ENOTDIR if path component is not a directory
 *             -EISDIR  if path is a directory (cannot unlink directory)
 *             -EINVAL  if otherwise bad path
 */
int fsx492_unlink(const char * path)
{
    fprintf(stdout, "fsx492_unlink: %s\n", path);
    assert(path);

    int ret = 0;
    uint32_t ino = 0, parent_ino = 0;
    if ((ret = LP(path, &ino, &parent_ino)) < 0) {
        return ret;
    }
    // inode and parent were found
    assert(ino);
    assert(parent_ino);

    // unlink entry
    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    if (S_ISDIR(ctx->inodes[ino].mode)) {
        return -EISDIR;
    }

    return _unlink(basename(path), parent_ino, ctx);
}


/**
 * @brief      remove a directory
 *
 * @param[in]  path  The path
 *
 * @return     0            on success
 *             -EIO         on disk error
 *             -ENOENT      if path does not exist
 *             -ENOTDIR     if any path component is not a directory
 *             -ENOTEMPTY   if directory is not empty
 *             -EINVAL      if otherwise bad path
 */
int fsx492_rmdir(const char * path)
{
    fprintf(stdout, "fsx492_rmdir: %s\n", path);
    assert(path);

    // TODO:
    //first make sure path acc exists and get its inode and parent inode
    uint32_t ino = 0, parent_ino = 0;
    int ret = 0;
    if ((ret = LP(path, &ino, &parent_ino)) < 0) {
        //some kinda error occured during lookup - which at least means path dne
        fprintf(stderr, "fsx492_rmdir: LP failed with %d\n", ret);
        return ret;
    }
    // confirm inode is directory
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    if (!S_ISDIR(ctx->inodes[ino].mode)) {
        fprintf(stderr, "fsx492_rmdir: target is not a directory\n");
        return -ENOTDIR;
    }

    // confirm directory is empty (only `.` and `..` entries) - so size should be 2 * sizeof(struct fsx492_dirent)
    // cuz rmdir aint meant to work on a non empty dir
    if (ctx->inodes[ino].size > 2 * sizeof(struct fsx492_dirent)) {
        fprintf(stderr, "fsx492_rmdir: directory is not empty\n");
        return -ENOTEMPTY;
    }
    //then we gotta make sure the only entries are '.' and '..'
    struct fsx492_dirent entries[FSX492_DIRENTRIES_PER_BLK];
    memset(entries, 0, sizeof(entries));
    for (int i=0; i<FSX492_DIRENTRIES_PER_BLK; i++) {//only goes thru first block cuz thats where i alloc the '.' and '..'
        if (read_blks(ctx->inodes[ino].direct_blks[0], 1, (void *)entries) < 0) {
            fprintf(stderr, "fsx492_rmdir: failed to read directory block\n");
            return -EIO;
        }
        if (entries[i].valid) {
            if (strcmp(entries[i].name, ".") != 0 && strcmp(entries[i].name, "..") != 0) {
                fprintf(stderr, "fsx492_rmdir: directory is not empty\n");
                return -ENOTEMPTY;
            }
        }
    }

    // remove `.` and `..` subdirectories - happens (essentially) at the end

    // unlink directory inode from parent
    if ((ret = _unlink(basename(path), parent_ino, ctx)) < 0) {
        fprintf(stderr, "fsx492_rmdir: failed to unlink directory from parent\n");
        return ret;
    }
    ctx->inodes[parent_ino].nlink--;//dec cuz parent's dir has 1 less link to it now that '..' is removed
    dirty_inode(parent_ino, ctx);

    //free dir's 1 block - manual cuz using truncate may have been what gave me errors
    if (ctx->inodes[ino].direct_blks[0] != 0) {
        free_blk(ctx->inodes[ino].direct_blks[0], ctx);
    }
    ctx->inodes[ino].blocks = 0;
    ctx->inodes[ino].size = 0;
    ctx->inodes[ino].direct_blks[0] = 0;
    dirty_inode(ino, ctx);

    return 0;
}


/**
 * @brief      change the size of a file
 *
 * @param[in]  path  The path
 * @param[in]  len   The length
 * @param      fi    { parameter_description }
 *
 * @return     0        on success
 *             -EIO     on disk error
 *             -EISDIR  path is a directory
 *             -ENOENT  file does not exist
 *             -ENOTDIR path component is not a directory
 * 
 * @note       relevant documentation from <fuse.h> included below
 * 
 * ~~~~~~~
 * 
 * `fi` will always be NULL if the file is not currently open, but
 * may also be NULL if the file is open.
 *
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is
 * expected to reset the setuid and setgid bits.
 */
int fsx492_truncate(const char * path, off_t len, struct fuse_file_info *fi)
{
    fprintf(stdout, "fsx492_truncate: %s %ld\n", path, len);
    assert(path);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    int ret = 0;
    uint32_t ino = 0;

    if (fi) {
        ino = ((struct fh *)fi->fh)->ino;
    } else if ((ret = LP(path, &ino, NULL)) < 0) {
        return ret;
    }

    if (S_ISDIR(ctx->inodes[ino].mode)) {
        return -EISDIR;
    }

    return _truncate(ino, 0, ctx);
}


/**
 * @brief      rename a path
 *
 * @param[in]  oldpath  The oldpath
 * @param[in]  newpath  The newpath
 * @param[in]  flags    The flags (ignored)
 *
 * @return     0        on success
 *             -EIO     on disk error
 *             -ENOENT  oldpath does not exist
 *             -ENOTDIR component of oldpath or newpath is not directory
 *             -EINVAL  component of oldpath or newpath is invalid
 */
int fsx492_rename(
    const char * oldpath, const char * newpath, unsigned int flags)
{
    fprintf(stdout, "fsx492_rename: %s %s\n", oldpath, newpath);
    assert(oldpath);
    assert(newpath);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    int ret = 0;
    uint32_t old_ino = 0, oldparent_ino = 0;
    uint32_t new_ino = 0, newparent_ino = 0;

    // unlink newpath if it already exists
    switch (ret = LP(newpath, &new_ino, &newparent_ino)) {
    case -EIO:          // disk error
    case -ENOTDIR:      // bad newpath
        return ret;
    case -ENOENT:
        if (!new_ino)   // bad newpath
            return ret;
        else            // no file at newpath
            break;
    case 0:             // found entry
        if ((ret = _unlink(basename(newpath), newparent_ino, ctx)) < 0) {
            assert(ret != -ENOENT);
            return ret;
        }
        break;
    default:            // unreachable
        assert(0);
    }

    assert(newparent_ino);

    // find oldpath
    if ((ret = LP(oldpath, &old_ino, &oldparent_ino)) < 0) {
        return ret;
    }
    assert(old_ino);
    assert(oldparent_ino);

    // link file to newpath
    if ((ret = _link(basename(newpath), old_ino, newparent_ino, ctx)) < 0) {
        return ret;
    }

    // unlink file from oldpath
    if ((ret = _unlink(basename(oldpath), oldparent_ino, ctx)) < 0) {
        return ret;
    }

    return 0;
}


/**
 * @brief      change file permissions
 *
 * @param[in]  path  The path
 * @param[in]  mode  The mode
 * @param      fi    The fuse file info (may be null)
 *
 * @return     0        success
 *             -ENOENT  path does not exist
 *             -ENOTDIR path component not directory
 *             -EINVAL  path component invalid
 *             
 * @note       largely useless until `open` and `opendir` perform
 *             permission checking
 */
int fsx492_chmod(const char * path, mode_t mode, struct fuse_file_info * fi)
{
    fprintf(stdout, "fsx492_chmod: %s (mode=%o)\n", path, mode);
    assert(path);


    //get inode number
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    int errcatch;
    uint32_t inodeID = 0;
    if (fi) inodeID = ((struct fh *)fi->fh)->ino;
    else if ((errcatch = LP(path, &ino, NULL)) < 0) return errcatch;

    //change permissions
    struct fsx492_inode * inode = ctx->inodes + ino
    inode->mode = (ctx->inodes[ino].mode & S_IFMT) | (mode & ~S_IFMT); //use SIFMT flag
    inode->ctime = time(NULL);
    dirty_inode(ino, ctx);
    return 0;
}


/**
 * @brief      change access and modification times of a file
 *             with nanosecond resolution (nanoseconds ignored)
 *
 * @param[in]  path  The path
 * @param[in]  tv    The timespec array
 * @param      fi    { parameter_description }
 *
 * @return     0        success
 *             -ENOENT  path does not exist
 *             -ENOTDIR path component not directory
 *             -EINVAL  path component not valid
 * 
 * @note       tv[0] contains the access time and tv[1] contains the
 *             modification time per utimensat(2) man page.
 *             relevant documentation from <fuse.h> included below
 * 
 * ~~~~~~~
 * 
 * This supersedes the old utime() interface.  New applications
 * should use this.
 *
 * `fi` will always be NULL if the file is not currently open, but
 * may also be NULL if the file is open.
 *
 * See the utimensat(2) man page for details.
 */
int fsx492_utimens(
    const char * path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    fprintf(stdout, "fsx492_utimens: %s\n", path);
    assert(path);

    int ret = 0;
    uint32_t ino = 0;

    if (fi) {
        ino = ((struct fh *)fi->fh)->ino;
    } else if ((ret = LP(path, &ino, NULL)) < 0) {
        return ret;
    }

    struct context * ctx = (struct context *)fuse_get_context()->private_data;

    time_t now = time(NULL);
    ctx->inodes[ino].atime = (tv[0].tv_nsec == UTIME_NOW) ? now : tv[0].tv_sec;
    ctx->inodes[ino].mtime = (tv[1].tv_nsec == UTIME_NOW) ? now : tv[1].tv_sec;

    return 0;
}


/**
 * @brief      get file system statistics
 *
 * @param[in]  path       The path
 * @param      st         The statvfs struct
 *
 * @return     0 (always succeeds)
 * 
 * @note       see statvfs(3)
 *             https://man7.org/linux/man-pages/man3/statvfs.3.html
 */
int fsx492_statfs(const char * path, struct statvfs * st)
{
    fprintf(stdout, "fsx492_statfs: %s\n", path);
    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    struct fsx492_superblk * sb = (struct fsx492_superblk *)ctx->metadata;

    memset(st, 0, sizeof(*st));
    st->f_bsize = FSX492_BLKSZ;
    st->f_frsize = FSX492_BLKSZ;
    st->f_blocks = (fsblkcnt_t)ctx->n_blocks;
    st->f_bfree = st->f_bavail = (fsblkcnt_t)count_avail_blks(ctx);
    st->f_files = (fsfilcnt_t)(sb->inode_region_sz * FSX492_INODES_PER_BLK);
    st->f_ffree = st->f_favail = (fsfilcnt_t)count_avail_inodes(ctx);
    st->f_fsid = sb->magic;
    st->f_namemax = FSX492_FILENAMESZ - 1;

    return 0;
}


/*EXTRA CREDIT*/
int fsx492_symlink(const char * path, const char * softpath) {

    fprintf(stdout, "fsx492_symlink: %s -> %s\n", softpath, path);
    assert(path);
    assert(softpath);

    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    int errcatch;

    // softpath's parent must exist, softpath itself must not. same idea as hardlink
    uint32_t new_ino = 0, parent_ino = 0;
    errcatch = LP(softpath, &new_ino, &parent_ino);
    if (!parent_ino)    return -ENOENT;
        if ((errcatch = LP(softpath, &new_ino, &new_parent_ino)) <=0) {
        if (errcatch == 0) return -EEXIST;     // "success" means failure here- should not be found
        if (errcatch != -ENOENT) return errcatch;     //ENOENT is *wanted* here
    };

    // check name length
    if (strlen(basename(softpath)) >= FSX492_FILENAMESZ) return -EINVAL;

    // allocate a new inode for the symlink
    uint32_t link_ino = 0;
    if ((errcatch = alloc_inode(&link_ino, ctx)) < 0) return errcatch;


    //set all inodes attributes
    struct fsx492_inode* inode = &ctx->inodes[link_ino];
    inode->ino   = link_ino;
    inode->mode  = S_IFLNK | 0777;
    inode->uid   = fuse_get_context()->uid;
    inode->gid   = fuse_get_context()->gid;
    inode->nlink = 1;

    //clear blocks
    inode->atime = time(NULL), inode->ctime = time(NULL), inode->mtime = time(NULL);
    for (int i = 1 ; i < FSX492_N_DIRECT;i++)  inode->direct_blk[i] =  0; 
    inode->indir1_blks = 0;
    inode->indir2_blks = 0;

    //write path to inode
    size_t pathlen = strlen(path) + 1;  
    uint32_t blkno = 0;
    if ((errcatch = alloc_blk(&blkno, ctx)) < 0) return errcatch;
    char blkbuf[FSX492_BLKSZ] = {0};
    memcpy(blkbuf, path, pathlen);
    if (write_blks(blkno, 1, blkbuf) < 0) return -EIO;
    
    //finalize and link
    inode->direct_blks[0] = blkno;
    inode->blocks = 1;
    inode->size= pathlen;
    dirty_inode(link_ino, ctx);
    return _link(basename(softpath), link_ino, parent_ino, ctx);
}

int fsx492_readlink(const char * path, char * buf, size_t size) {
    fprintf(stdout, "fsx492_readlink: %s\n", path);
    assert(path);

    struct context * ctx = (struct context *)fuse_get_context()->private_data;
    int errcatch;

    //locate specified inode at path
    uint32_t ino = 0;
    if ((errcatch = LP(path, &ino, NULL)) < 0) return errcatch;
    struct fsx492_inode* inode = &ctx->inodes[ino];
    if (!S_ISLNK(inode->mode)) return -EINVAL;

    // resolve target path
    char blkbuf[FSX492_BLKSZ];
    if (read_blks(inode->direct_blks[0], 1, blkbuf) < 0) return -EIO;
    strncpy(buf, blkbuf, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

