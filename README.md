# C-File-System-Implementation
## Overview
Implement a simple extent-based file system for the course "Operating System" from University of Toronto. (Received a mark of 93/100)\
An extent is a contiguous set of blocks allocated to a file, and is defined by the starting block number and the number of blocks in the extent.\
A single extent is often not sufficient, so a file may be composed of a sequence of extents.

## Functionality
- Design a file system that support the following file system operations:
    * Formatting the disk image (mkfs)
    * Creating and deleting directories (mkdir, rmdir)
    * Creating and deleting files (creat, unlink)
    * Displaying metadata about a file or directory (stat)
    * Setting the size of a file (truncate)
    * Writing data to files and reading data from files (read, write)
- Use userspace filesystem framework `FUSE` and its kernel module to receive file system calls and pass them to the implemented file system program
- Run FUSE on `Ubuntu Virtual Machine`
- Debug program using `gdb`
- Include a shell script file `runit.sh` to exercise the functionality

## Important files
- Entry point of the program - `mkfs.c`
- Implementation - `a1fs.c`
- Shell scripts - `runit.sh`
- Implementation explanation - `Project Layout.pdf`

## Shell Scripts
### make sure the mountpoint is not in used
fusermount -u ${root}

### create an image
truncate -s ${image_size} ${image}

### compile
make

### format the image
./mkfs.a1fs -i ${inodes} ${image}

### mount the image
./a1fs ${image} ${root}

### create a directory
mkdir ${root}/d1

### display contents of the directory which should be empty at this point
ls ${root}

### create a file
truncate -s 5 ${root}/d1/f1

### add data to the file
echo "hello there" >> ${root}/d1/f1

### print contents of the file
cat ${root}/d1/f1

### display more information about the directory
ls -la ${root}/d1

### create another file by the truncate command
truncate -s 5000 ${root}/f2

### change size of the file and check if its metadata has updated
truncate -s 100 ${root}/f2
stat ${root}/f2

### add more data to a file and read file
echo "how are you" >> ${root}/d1/f1
cat ${root}/d1/f1

### remove a file and check if the removal is successful
unlink ${root}/d1/f1
ls -la ${root}/d1

### create and remove a directory, displaying its contents now should throw errors
mkdir ${root}/d1/d2
rmdir ${root}/d1/d2
ls ${root}/d1/d2

### display information about the file system
stat -f ${root}

### unmount the file system
fusermount -u ${root}

### mount the file system again
./a1fs ${image} ${root}

### check the contents of the file system
stat -f ${root}
ls ${root}
cat ${root}/d1/f1

### unmount the file system
fusermount -u ${root}

## Documentation
File `Project Layout.pdf` explains in thorough detail:
- Data structures used to implement the file system
- Layout of data on the disk
- Several of the important algorithms needed to implement basic file system functionality
