/**
 * file:        blkdev.h
 * description: emulates block device access to image
 *              
 * credit:
 *  Peter Desnoyers, November 2016
 *  Philip Gust, March 2019
 *  Phillippe Meunier, 2020-2025
 *  Ryan Tsang, 2026
 */

#ifndef __BLKDEV_H__
#define __BLKDEV_H__

#include <stdint.h>

/**
 * block device block size
 */
enum { BLKDEV_BLKSZ = 1024 };

/**
 * block device operation status codes
 */
enum {
	BLKDEV_SUCCESS   =  0,
	BLKDEV_E_BADADDR = -1,	// bad block address
	BLKDEV_E_UNAVAIL = -2,  // device unavailable
	BLKDEV_E_FAULT   = -3,  // internal error
	BLKDEV_E_BADDEV  = -4,  // bad device
};

/**
 * block device definition
 */
struct blkdev {
	struct blkdev_ops *ops;	// block device vtable
	void * private;			// block device private state
};

/**
 * block device operations
 */
struct blkdev_ops {
	// get number of blocks in block device
	int (*size)(struct blkdev *);

	// beginning with start, read n blocks from block device to buf
	int (*read)(struct blkdev *, uint32_t start, uint32_t n, void * buf);

	// beginning with start, write n blocks from buf to block device
	int (*write)(struct blkdev *, uint32_t start, uint32_t n, void * buf);

	// flush the block device buffers
	int (*flush)(struct blkdev *, uint32_t start, uint32_t n);

	// close the block device
	void (*close)(struct blkdev *);
};

/**
 * @brief      initialize a block device from an image path
 *
 * @param      dev      The dev
 * @param      imgpath  The imgpath
 *
 * @return     BLKDEV_SUCCESS on success
 *             BLKDEV_E_BADDEV on any of the following:
 *               - failed to allocate space for image struct
 *               - failed to open image file
 *               - image file is not a multiple of block size
 */
int blkdev_init(struct blkdev * dev, char * imgpath);


#endif // __BLKDEV_H__