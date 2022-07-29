# constants
root="/tmp/hejingne"
image="img"
image_size=262144
inodes=16


# make sure the mountpoint is not in used
fusermount -u ${root}

# create an image
truncate -s ${image_size} ${image}

# compile
make

# format the image
./mkfs.a1fs -i ${inodes} ${image}

# mount the image
./a1fs ${image} ${root}

# create a directory
mkdir ${root}/d1

# display contents of the directory which should be empty at this point
ls ${root}

# create a file
truncate -s 5 ${root}/d1/f1

# add data to the file
echo "hello there" >> ${root}/d1/f1

# print contents of the file
cat ${root}/d1/f1

# display more information about the directory
ls -la ${root}/d1

# create another file by the truncate command
truncate -s 5000 ${root}/f2

# change size of the file and check if its metadata has updated
truncate -s 100 ${root}/f2
stat ${root}/f2

# add more data to a file and read file
echo "how are you" >> ${root}/d1/f1
cat ${root}/d1/f1

# remove a file and check if the removal is successful
unlink ${root}/d1/f1
ls -la ${root}/d1

# create and remove a directory, displaying its contents now should throw errors
mkdir ${root}/d1/d2
rmdir ${root}/d1/d2
ls ${root}/d1/d2

# display information about the file system
stat -f ${root}

# unmount the file system
fusermount -u ${root}

# mount the file system again
./a1fs ${image} ${root}

# check the contents of the file system
stat -f ${root}
ls ${root}
cat ${root}/d1/f1

# unmount the file system
fusermount -u ${root}
