/**
 * file:        main.c
 * description: the fuse client
 *              mounts the cs492 file system
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
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <fuse.h>

#include "blkdev.h"
#include "fsx492.h"

#define DEFAULT_IMGNAME "data/test.img"

// fsx492 callback bindings
extern struct fuse_operations fsx492_ops;

// global pointer to block device
struct blkdev * disk = &(struct blkdev){ .ops = NULL, .private = NULL };

// see "libfuse/include/fuse_opt.h" for reference
// see "libfuse/example/hello.c" for example

static struct options {
    char * imgname;
    int show_help;
} options;

#define CLI_OPT(t, p) { t, offsetof(struct options, p), 1 }

static struct fuse_opt opts[] = {
    CLI_OPT("--img %s", imgname),
    CLI_OPT("-h", show_help),
    CLI_OPT("--help", show_help),
    FUSE_OPT_END
};

/**
 * @brief      Shows the help.
 *
 * @param[in]  prog  The prog
 */
static void show_help(const char * prog)
{
    printf("usage: %s [options] <mountpoint>\n\n", prog);
    printf("fsx492-specific options:\n"
           "    --img <s>       path to image file to load (required)\n"
           "                    (default: \"" DEFAULT_IMGNAME "\")\n"
           "\n");
}


int main(int argc, char **argv)
{
    int ret = 0;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    // set string defaults
    // strdup needed since fuse_opt_parse frees defaults
    options.imgname = strdup(DEFAULT_IMGNAME);

    if (fuse_opt_parse(&args, &options, opts, NULL) == -1) {
        return 1;
    }

    if (options.show_help) {
        // on "--help", print fsx492 help before fuse_main help
        // add back "--help" for fuse_main
        show_help(argv[0]);
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';
    } else if (blkdev_init(disk, options.imgname) < 0) {
        fprintf(stderr, "blkdev_init failed: %s\n", options.imgname);
        ret = 1; goto end;
    }

    ret = fuse_main(args.argc, args.argv, &fsx492_ops, NULL);

    // if (disk->private) {
    //     disk->ops->close(disk);
    // }

end:
    fuse_opt_free_args(&args);
    return ret;
}