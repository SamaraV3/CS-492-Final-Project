/**
 * file:        blkdev.c
 * description: blkdev implementation
 *              
 * credit:
 *  Peter Desnoyers, November 2016
 *  Philip Gust, March 2019
 *  Phillippe Meunier, 2020-2025
 *  Ryan Tsang, 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "blkdev.h"


/**
 * @brief      backing image metadata
 */
struct image {
    char * path;    // path to image file
    int fd;         // open file descriptor
    int size;       // number of blocks in device
};


/**
 * @brief      gets the size of the block device
 *
 * @param      dev   The block device
 *
 * @return     the number of block in device
 */
static int blkdev_size(struct blkdev * dev)
{
    assert(dev);
    struct image * im = dev->private;
    assert(im);

    // TODO:

    return im->size;
}

/**
 * @brief      reads blocks from the block device
 *
 * @param      dev    The block device
 * @param[in]  start  The starting block
 * @param[in]  n      The number of blocks to read
 * @param      buf    The buffer to read to
 *
 * @return     BLKDEV_SUCCESS on success
 *             BLKDEV_E_BADADDR if any blocks do not exist (don't read)
 *             BLKDEV_E_UNAVAIL if image file not opened
 *             BLKDEV_E_FAULT on failure to read or short read from file
 */
static int blkdev_read(
    struct blkdev * dev, uint32_t start, uint32_t n, void * buf)
{
    assert(dev);
    struct image * im = dev->private;
    assert(im);

    // TODO:

    // check if unavailable

    // check block range

    // read blocks
    
    return BLKDEV_E_FAULT;
}


/**
 * @brief      write blocks to the block device
 *
 * @param      dev    The block device
 * @param[in]  start  The starting block
 * @param[in]  n      The number of blocks to write
 * @param      buf    The buffer to write from
 *
 * @return     BLKDEV_SUCCESS on success
 *             BLKDEV_E_BADADDR if any blocks do not exist (don't write)
 *                              or if attempting to write to superblock
 *             BLKDEV_E_UNAVAIL if image file not opened
 *             BLKDEV_E_FAULT on failure to write or short write to file
 */
static int blkdev_write(
    struct blkdev * dev, uint32_t start, uint32_t n, void * buf)
{
    assert(dev);
    struct image * im = dev->private;
    assert(im);

    // TODO:

    // check if unavailable
    
    // check block range

    // write blocks

    return BLKDEV_E_FAULT;
}


/**
 * @brief      flush the block device
 *             (does nothing because no internal buffers)
 *
 * @param      dev    The block device
 * @param[in]  start  The starting block
 * @param[in]  n      The number of blocks to flush
 *
 * @return     BLKDEV_SUCCESS on success
 *             BLKDEV_E_UNAVAIL if image file not opened
 */
static int blkdev_flush(struct blkdev * dev, uint32_t start, uint32_t n)
{
    assert(dev);
    struct image * im = dev->private;
    assert(im);

    // TODO:

    return BLKDEV_E_FAULT;
}


/**
 * @brief      closes the block device (if available)
 *
 * @param      dev   The block device
 * 
 * @note       this function must perform the following:
 *               - close the image file if opened,
 *                 setting the fd to -1 if it did so
 *               - free any allocated fields within the blkdev struct,
 *                 setting them to NULL if freed
 * 
 * @note       this function should NOT unlink the vtable
 *             this function should NOT free dev (caller's responsibility)
 */
static void blkdev_close(struct blkdev * dev)
{
    assert(dev);
    struct image * im = dev->private;
    assert(im);

    // TODO:

    // close image file

    // free allocated memory

}

/**
 * blkdev vtable mapping
 */
static struct blkdev_ops ops = {
    .size  = blkdev_size,
    .read  = blkdev_read,
    .write = blkdev_write,
    .flush = blkdev_flush,
    .close = blkdev_close,
};


/// see header for docs
int blkdev_init(struct blkdev * dev, char * imgpath)
{
    fprintf(stdout, "blkdev_init: %s\n", imgpath);
    assert(dev);
    assert(imgpath);

    // allocate and initialize image
    struct image * im = malloc(sizeof(*im));
    if (!im) {
        return BLKDEV_E_BADDEV;
    }

    im->path = strdup(imgpath);

    if ((im->fd = open(imgpath, O_RDWR)) < 0) {
        fprintf(stderr, "can't open image %s: %s\n", imgpath, strerror(errno));
        return BLKDEV_E_BADDEV;
    }

    // access image
    struct stat sb;
    if (fstat(im->fd, &sb) < 0) {
        fprintf(stderr, "can't access image %s: %s\n", imgpath, strerror(errno));
        return BLKDEV_E_BADDEV;
    }
    // todo: should add a check that this is a regular file

    // check that file is a multiple of block size
    if (sb.st_size % BLKDEV_BLKSZ) {
        fprintf(stderr, "image size is not a multiple of block size %d: %s\n",
            BLKDEV_BLKSZ, imgpath);
        return BLKDEV_E_BADDEV;
    }

    im->size = sb.st_size / BLKDEV_BLKSZ;

    fprintf(stderr, "blkdev_init: image { fd=%d, size=%d }\n",
        im->fd, im->size);

    dev->private = (void *)im;
    dev->ops = &ops;

    return BLKDEV_SUCCESS;
}