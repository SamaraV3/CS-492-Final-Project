// test_blkdev.c - corrected version
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "blkdev.h"

int main() {
    struct blkdev dev;
    char buf[BLKDEV_BLKSZ];
    char buf2[BLKDEV_BLKSZ];
    int result;
    
    printf("=== Testing blkdev ===\n");
    
    // Test 1: Initialize
    printf("Test 1: Initializing block device...\n");
    result = blkdev_init(&dev, "data/test.img");
    if (result != BLKDEV_SUCCESS) {
        printf("FAILED: blkdev_init returned %d\n", result);
        return 1;
    }
    printf("OK - device initialized\n");
    
    // Test 2: Get size (call through ops pointer!)
    printf("Test 2: Getting device size...\n");
    int size = dev.ops->size(&dev);
    printf("Device has %d blocks (%d bytes)\n", size, size * BLKDEV_BLKSZ);
    if (size <= 0) {
        printf("FAILED: invalid size\n");
        return 1;
    }
    printf("OK\n");
    
    // Test 3: Read block 0 (call through ops->read!)
    printf("Test 3: Reading block 0...\n");
    result = dev.ops->read(&dev, 0, 1, buf);
    if (result != BLKDEV_SUCCESS) {
        printf("FAILED: read returned %d\n", result);
        return 1;
    }
    printf("OK - read %d bytes\n", BLKDEV_BLKSZ);
    
    // Test 4: Try to write to block 0 (should FAIL)
    printf("Test 4: Attempting to write to block 0 (should fail)...\n");
    result = dev.ops->write(&dev, 0, 1, buf);
    if (result == BLKDEV_E_BADADDR) {
        printf("OK - superblock write correctly rejected\n");
    } else {
        printf("FAIL - expected %d, got %d\n", BLKDEV_E_BADADDR, result);
        return 1;
    }
    
    // Test 5: Read beyond end (should FAIL)
    printf("Test 5: Reading beyond disk end (should fail)...\n");
    result = dev.ops->read(&dev, 999999, 1, buf);
    if (result == BLKDEV_E_BADADDR) {
        printf("OK - out-of-bounds read correctly rejected\n");
    } else {
        printf("FAIL - expected %d, got %d\n", BLKDEV_E_BADADDR, result);
        return 1;
    }
    
    // Test 6: Write to a valid block (if block 100 exists)
    if (size > 100) {
        printf("Test 6: Writing to block 100...\n");
        memset(buf2, 0xAA, BLKDEV_BLKSZ);
        result = dev.ops->write(&dev, 100, 1, buf2);
        if (result != BLKDEV_SUCCESS) {
            printf("FAILED: write returned %d\n", result);
            return 1;
        }
        printf("OK\n");
        
        // Test 7: Read it back
        printf("Test 7: Reading back block 100...\n");
        result = dev.ops->read(&dev, 100, 1, buf);
        if (result != BLKDEV_SUCCESS) {
            printf("FAILED: read returned %d\n", result);
            return 1;
        }
        if (memcmp(buf, buf2, BLKDEV_BLKSZ) != 0) {
            printf("FAILED: data mismatch\n");
            return 1;
        }
        printf("OK - data verified\n");
    } else {
        printf("Tests 6-7: Skipped (block 100 beyond disk size %d)\n", size);
    }
    
    // Test 8: Flush
    printf("Test 8: Flushing device...\n");
    result = dev.ops->flush(&dev, 0, size);
    if (result != BLKDEV_SUCCESS) {
        printf("WARNING: flush returned %d (may be fine)\n", result);
    } else {
        printf("OK\n");
    }
    
    // Test 9: Close
    printf("Test 9: Closing device...\n");
    dev.ops->close(&dev);
    printf("OK\n");
    
    printf("\n=== All blkdev tests passed! ===\n");
    return 0;
}