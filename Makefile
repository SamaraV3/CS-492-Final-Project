CC := gcc
CFLAGS := -g -Wall -fmessage-length=0 -D_FILE_OFFSET_BITS=64
INCLUDES := -Iinclude
FUSE := $(shell pkg-config fuse3 --cflags --libs)

SRCS = \
	src/blkdev.c \
	src/fsx492.c

TARGET ?= fsx492

.PHONY: all clean
all: $(TARGET)

clean:
	rm -f $(TARGET) *.o *~ core

fsx492: main.c $(SRCS)
	$(CC) $(CFLAGS) $^ $(FUSE) -o $@ $(INCLUDES)


.PHONY: reset mount
reset: data/test.img.bkp
	cp data/test.img.bkp data/test.img
	mount | grep -e "fsx492" && fusermount -u ./testfs || true
	-rm -r ./testfs
	mkdir ./testfs

mount:
	./fsx492 -d -s --img data/test.img ./testfs


SUBMISSION_FILES := \
	data/gospels.txt \
	data/lorem.txt \
	data/test.img \
	data/test.img.bkp \
	include/blkdev.h \
	include/fsx492.h \
	src/blkdev.c \
	src/fsx492.c \
	main.c \
	Makefile \
	README.md \
	test.py \
	screenshots.pdf 

.PHONY: submission
submission: project.zip

project.zip: $(SUBMISSION_FILES)
	zip -r $@ $^
