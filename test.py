#!/usr/bin/env python3
import os
import subprocess
import threading
import time
import tempfile
import shutil
import signal
import sys
from random import randint

# -------- CONFIG --------
FUSE_BINARY = "fsx492"          # your compiled FS
FUSE_ARGS = ["-f", "-d", "-s"]  # run single threaded debug mode
MOUNT_TIMEOUT = 10              # seconds
# ------------------------


##############################################################################
# BEGIN TEST DEFINITIONS
##############################################################################

# define tests below by creating functions that are prefixed with "test_"


def test_basic(mountpoint):

    # TEST: directory listing

    print(f"[test] list {mountpoint}")
    entries = os.listdir(mountpoint)
    print(entries)
    assert "hello.txt" in entries, "readdir missing file"

    # TEST: file existence
    path = os.path.join(mountpoint, "hello.txt")
    print(f"[test] file existence: {path}")
    assert os.path.exists(path), "file missing"

    # TEST: read
    print(f"[test] read {path}")
    with open(path, "r") as f:
        data = f.read()
    assert "hello" in data, "unexpected file content"

    # TEST: partial read
    print(f"[test] partial read {path}")
    with open(path, "r") as f:
        f.seek(6)
        data = f.read()
    assert "world" in data, "partial read failed"

    # TEST: out of bounds read
    print(f"[test] out of bounds read {path}")
    with open(path, "r") as f:
        f.seek(30)
        data = f.read()
    assert len(data) == 0, "out of bounds read should return nothing"

    # TEST: stat
    print(f"[test] stat {path}")
    st = os.stat(path)
    assert st.st_size == len("hello world!\n"), "invalid file size"

    print("[test] passed basic")


def test_large_file(mountpoint):

    # TEST: large file copy
    src = "./data/gospels.txt"
    assert os.path.exists(src), "src not found: {}".format(src)

    dst = f"{mountpoint}/{os.path.basename(src)}"
    shutil.copy(src, dst)
    assert os.path.exists(dst), "copy failed: {} does not exist".format(dst)

    with open(src, 'rb') as f:
        srcdata = f.read()

    with open(dst, 'rb') as f:
        dstdata = f.read()

    assert len(srcdata) == len(dstdata), \
        "length check failed: {} (src) != {} (dst)".format(
            len(srcdata), len(dstdata))

    diff = -1
    for i in range(len(srcdata)):
        if srcdata[i] != dstdata[i]:
            diff = i
            break

    assert diff < 0, "data different @ {}:\nsrc: {}\ndst: {}".format(
        diff, srcdata[diff:diff+10], dstdata[diff:diff+10])

    print("[test] passed large file")

# test for finding entries in a directory block
def test_directory_lookup(mountpoint):
    """Test find_entry functionality through directory operations"""
    
    # Test 1: Root directory should have '.' and '..'
    print(f"[test] Checking root directory entries")
    entries = os.listdir(mountpoint)
    print(f"  Root entries: {entries}")
    
    # Test 2: Create a test directory and file
    test_dir = os.path.join(mountpoint, "testdir")
    print(f"[test] Creating directory: {test_dir}")
    os.mkdir(test_dir)
    
    test_file = os.path.join(test_dir, "hello.txt")
    print(f"[test] Creating file: {test_file}")
    with open(test_file, "w") as f:
        f.write("test content")
    
    # Test 3: Verify they exist (this will use find_entry via lookup_path)
    print(f"[test] Verifying directory exists")
    assert os.path.exists(test_dir), "Directory not found"
    assert os.path.isdir(test_dir), "Not a directory"
    
    print(f"[test] Verifying file exists")
    assert os.path.exists(test_file), "File not found"
    
    # Test 4: List directory contents
    print(f"[test] Listing testdir contents")
    dir_entries = os.listdir(test_dir)
    print(f"  {test_dir} entries: {dir_entries}")
    assert "hello.txt" in dir_entries, "File not in directory listing"
    
    # Test 5: Try to find non-existent file (should fail gracefully)
    bad_path = os.path.join(test_dir, "nonexistent.txt")
    print(f"[test] Verifying non-existent file is not found")
    assert not os.path.exists(bad_path), "Non-existent file reported as existing"
    
    # Test 6: Check parent directory reference works
    parent_entry = os.path.join(test_dir, "..")
    print(f"[test] Checking '..' resolves correctly")
    assert os.path.exists(parent_entry), "'..' entry missing"
    
    print("[test] passed directory lookup")

#test 1: adding and removing files from subdirs
def test_subdir_files(mountpoint):
    """Test creating and deleting files in subdirectories"""
    #create subdir
    newdir = os.path.join(mountpoint, "dir4")
    os.mkdir(newdir)
    subdir = os.path.join(newdir, "subdir1")
    os.mkdir(subdir)
    #create file in subdir
    subfile = os.path.join(subdir, "file1.txt")
    with open(subfile, "w") as f:
        pass
    #check file exists
    assert os.path.exists(subfile), "file not found in subdir"
    #delete it
    os.unlink(subfile)
    assert not os.path.exists(subfile), "file still exists after deletion"
    #cleanup
    os.rmdir(subdir)
    os.rmdir(newdir)
    assert not os.path.exists(subdir), "subdir still exists after deletion"
    assert not os.path.exists(newdir), "newdir still exists after deletion"
    print("[test] passed subdir files")

#test 2: adding and removing more than a block's worth of directories (at once)
def test_many_dirs(mountpoint):
    """Test creating and removing more than a block's worth of directories at once"""
    #each block holds 32 direntries so create 40
    entries_per_block = 32
    num_dirs = entries_per_block + 8
    dirnames = []
    test_root = os.path.join(mountpoint, "many_dirs_test_root")
    os.mkdir(test_root)

    #Create dirs
    print(f"[test] Creating {num_dirs} directories....")
    for i in range(num_dirs):
        dirname = os.path.join(test_root, f"many_dir_{i:02d}")
        os.mkdir(dirname)
        dirnames.append(dirname)
    
    #verify all exist
    entries = set(os.listdir(test_root))
    for dirname in dirnames:
        basename = os.path.basename(dirname)
        assert basename in entries, f"missing directory: {basename}"

    #remove all
    print(f"[test] Removing {num_dirs} directories....")
    for dirname in dirnames:
        os.rmdir(dirname)

    #verify theyre gone
    entries = set(os.listdir(test_root))
    for dirname in dirnames:
        basename = os.path.basename(dirname)
        assert basename not in entries, f"Directory {basename} still exists"
    os.rmdir(test_root)
    assert not os.path.exists(test_root), "test root still exists after deletion"

    print("[test] passed many directories")

#test 3:  overwriting a file (see `open` behavior)
def test_overwriting_file(mountpoint):
    """Test opening a file and writing to it"""
    fname =  os.path.join(mountpoint,"ow.txt")
    with open(fname,"w") as f:
        f.write("This is the file contents before overwriting")
    with open(fname,"r") as f:
        for line in f:
            assert (line == "This is the file contents before overwriting"), "Initial write: failure"

    with open(fname,"w") as f:
        f.write("This is the file contents after overwriting")    
    with open(fname,"r") as f:
        for line in f:
            assert(line == "This is the file contents after overwriting"), "Overwrite: failure"
        
    os.unlink(fname)
    print("[test] passed overwriting file")

#test 4:  opening a file in "append" mode (see `open` behavior)
def test_appending_file(mountpoint):
    """Test opening a file and appending to it"""
    fname =  os.path.join(mountpoint,"ap.txt")
    with open(fname,"w") as f:
        f.write("This is the file contents before appending. ")
    with open(fname,"r") as f:
        for line in f:
            assert (line == "This is the file contents before appending. "), "Initial write: failure"

    with open(fname,"a") as f:
        f.write("This is the file contents after appending.")    
    with open(fname,"r") as f:
        for line in f:
            assert(line == "This is the file contents before appending. This is the file contents after appending."), "Append: failure"
        
    os.unlink(fname)
    print("[test] passed appending file")

#test 5: counting hard links
def test_counting_hlink(mountpoint):
    basef =  os.path.join(mountpoint,"hl.txt")
    with open(basef,"w") as f:
        pass

    for i in range(0,3):
        lname = os.path.join(mountpoint,"L" + str(i))
        os.link(basef,lname)
        assert os.stat(basef).st_nlink == os.stat(lname).st_nlink == (i+1), "Link: failure to count hard links"

    for i in range(3,0,-1):
        os.unlink(os.path.join(mountpoint,"L" + str(i)))      
        assert os.stat(basef).st_nlink  == (i+1), "Unlink: failure to count hard links"

    os.unlink(basef)
    print("[test] passed counting hard links of file")
       
   
#test 6: update access/mod time
def test_time_update(mountpoint):
    """Test that access and mod times are updated correctly"""
    import time
    testfile = os.path.join(mountpoint, "time_test_temp.txt")
    with open(testfile, "w") as f:
        pass

    time.sleep(0.1)
    #init times
    st1 = os.stat(testfile)
    time.sleep(1)

    #then update access time ONLY - not mod time
    os.utime(testfile, (time.time(), st1.st_mtime))
    st2 = os.stat(testfile)
    assert st2.st_atime > st1.st_atime, "access time not updated"
    assert st2.st_mtime == st1.st_mtime, "mod time should not change"

    #cleanup
    os.unlink(testfile)
    print("[test] passed time update")

#test 7: changing permissions
def test_changing_permissions(mountpoint):
    """Test chmod"""
    fname = os.path.join(mountpoint, "ch.txt")
    with open(fname, "w") as f:
        pass
    for _ in range(0,5):
        targetperm = randint(0,7)*64 + randint(0,7)*8 + randint(0,7) ;
        os.chmod(fname,targetperm)
        realperm = os.stat(fname).st_mode &  (7*64 + 7*8  +7)
        assert (targetperm == realperm), "Permission change failed"
    
    os.unlink(fname)
    print("[test] passed changing permissions")
    
        


##############################################################################
# END TEST DEFINITIONS
##############################################################################

TESTS = {
    k[5:] : v for k, v in globals().items() if k.startswith('test_')
}


def reset_mount(mountpoint, fs_name=FUSE_BINARY):
    """reset fuse filesystem mountpoint after failure"""
    result = subprocess.run(
        ['mount'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False)

    if fs_name in result.stdout:
        subprocess.run(
            ['fusermount', '-u', mountpoint],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False)

    try:
        shutil.rmtree(mountpoint)
    except Exception:
        pass

    os.makedirs(mountpoint, exist_ok=True)

def is_mounted(mountpoint, fs_name=None):
    mountpoint = os.path.abspath(mountpoint)

    try:
        with open("/proc/self/mounts", "r") as f:
            lines = [line.strip() for line in f.readlines()]

        for line in lines:
            parts = line.split()
            if len(parts) < 3:
                continue

            dev, mnt, fstype = parts[:3]

            if os.path.abspath(mnt) == mountpoint:
                if fs_name is None:
                    return True
                if fs_name in dev or fs_name in fstype:
                    return True
        return False
    except Exception:
        return False


def wait_for_mount(mountpoint, timeout=MOUNT_TIMEOUT):
    """Wait until mountpoint is ready by probing it."""
    start = time.time()
    while time.time() - start < timeout:
        if is_mounted(mountpoint, fs_name="fsx492"):
            return True
        time.sleep(0.1)
    return False


def run_filesystem(mountpoint, ready_event, stop_event, logfile="fsx492.log"):
    """Run the FUSE filesystem."""
    cmd = ['stdbuf', '-oL', '-eL'] + [f"./{FUSE_BINARY}"] + FUSE_ARGS + [mountpoint]

    # unmount file system if needed first
    reset_mount(mountpoint)

    log = open(logfile, 'w')
    proc = subprocess.Popen(
        cmd,
        stdout=log,
        stderr=subprocess.STDOUT,
        text=True
    )

    # Wait until mount is ready
    if wait_for_mount(mountpoint):
        print("[fs] mounted")
        ready_event.set()
    else:
        print("[fs] mount timeout")
        proc.terminate()
        return

    # Keep process alive until stop_event
    while not stop_event.is_set():
        if proc.poll() is not None:
            print("[fs] process exited early!")
            return
        time.sleep(0.2)

    log.close()
    print("[fs] shutting down...")
    proc.send_signal(signal.SIGINT)

    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def run_tests(test, mountpoint, ready_event, stop_event):
    """Run filesystem tests."""
    ready_event.wait()

    print(f"[test] starting test: {test}")

    try:
        TESTS[test](mountpoint)
    except AssertionError as e:
        print(f"[test] FAILED: {e}")
    finally:
        stop_event.set()


if __name__ == "__main__":
    DEFAULT_MOUNTPOINT = './testfs'
    DEFAULT_IMAGE = 'data/test.img'
    import argparse
    parser = argparse.ArgumentParser('test.py',
        description="test script for fsx492")
    parser.add_argument('test', type=str, default='basic',
        help=f"options: {','.join(TESTS.keys())}")
    parser.add_argument('--mountpoint', type=str, default=DEFAULT_MOUNTPOINT,
        help=f"the path to mount at (default {DEFAULT_MOUNTPOINT})")
    parser.add_argument('--img', type=str, default='data/test.img',
        help=("the path to the image file, which will be restored from backup "
            f"(default: {DEFAULT_IMAGE})"))

    args = parser.parse_args()

    mountpoint = args.mountpoint
    assert args.test in TESTS, "test not found: {}".format(args.test)
    assert callable(TESTS[args.test]), "not callable: {}".format(args.test)

    imgpath = args.img
    assert os.path.exists(imgpath), "file not found: {}".format(imgpath)
    imgbkp = f"{imgpath}.bkp"
    assert os.path.exists(imgbkp), "could not find backup: {}".format(imgbkp)

    print(f"[main] cwd: {os.getcwd()}")
    print(f"[main] mountpoint: {mountpoint}")
    print(f"[main] restoring {imgpath} from {imgbkp}")
    shutil.copy(imgbkp, imgpath)

    ready_event = threading.Event()
    stop_event = threading.Event()

    fs_thread = threading.Thread(
        target=run_filesystem,
        args=(mountpoint, ready_event, stop_event),
        daemon=True
    )

    test_thread = threading.Thread(
        target=run_tests,
        args=(args.test, mountpoint, ready_event, stop_event),
        daemon=True
    )

    fs_thread.start()
    test_thread.start()

    test_thread.join()
    stop_event.set()
    fs_thread.join()

    # Try to unmount (Linux)
    print("[main] unmounting...")
    subprocess.run(["fusermount", "-u", mountpoint],
                   stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)

    shutil.rmtree(mountpoint)
    print("[main] done")


