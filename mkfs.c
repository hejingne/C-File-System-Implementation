/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2019, 2021 Karen Reid
 */

/**
 * CSC369 Assignment 1 - a1fs formatting tool.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "a1fs.h"
#include "map.h"
#include <time.h>

/** Command line options. */
typedef struct mkfs_opts {
	/** File system image file path. */
	const char *img_path;
	/** Number of inodes. */
	size_t n_inodes;

	/** Print help and exit. */
	bool help;
	/** Overwrite existing file system. */
	bool force;
	/** Zero out image contents. */
	bool zero;

} mkfs_opts;

static const char *help_str = "\
Usage: %s options image\n\
\n\
Format the image file into a1fs file system. The file must exist and\n\
its size must be a multiple of a1fs block size - %zu bytes.\n\
\n\
Options:\n\
    -i num  number of inodes; required argument\n\
    -h      print help and exit\n\
    -f      force format - overwrite existing a1fs file system\n\
    -z      zero out image contents\n\
";

static void print_help(FILE *f, const char *progname)
{
	fprintf(f, help_str, progname, A1FS_BLOCK_SIZE);
}


static bool parse_args(int argc, char *argv[], mkfs_opts *opts)
{
	char o;
	while ((o = getopt(argc, argv, "i:hfvz")) != -1) {
		switch (o) {
			case 'i': opts->n_inodes = strtoul(optarg, NULL, 10); break;

			case 'h': opts->help  = true; return true;// skip other arguments
			case 'f': opts->force = true; break;
			case 'z': opts->zero  = true; break;

			case '?': return false;
			default : assert(false);
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Missing image path\n");
		return false;
	}
	opts->img_path = argv[optind];

	if (!opts->n_inodes) {
		fprintf(stderr, "Missing or invalid number of inodes\n");
		return false;
	}
	return true;
}


/** Determine if the image has already been formatted into a1fs. */
static bool a1fs_is_present(void *image)
{
	//TODO: check if the image already contains a valid a1fs superblock
	const struct a1fs_superblock *sb = (const struct a1fs_superblock *)image;
	return sb->magic == A1FS_MAGIC;
}



/**
 * Format the image into a1fs.
 *
 * NOTE: Must update mtime of the root directory.
 *
 * @param image  pointer to the start of the image.
 * @param size   image size in bytes.
 * @param opts   command line options.
 * @return       true on success;
 *               false on error, e.g. options are invalid for given image size.
 */
static bool mkfs(void *image, size_t size, mkfs_opts *opts)
{
	//TODO: initialize the superblock and create an empty root directory
	//NOTE: the mode of the root directory inode should be set to S_IFDIR | 0777

	// Write to disk Image
		// 1. Superblock
	struct a1fs_superblock *sb = (struct a1fs_superblock *)image;

			// Initialize superblock
	sb->magic = A1FS_MAGIC;
	sb->size = size;
	sb->inodes_count = opts->n_inodes;
	sb->blocks_count = (size % A1FS_BLOCK_SIZE == 0)
			? size / A1FS_BLOCK_SIZE
			: (size / A1FS_BLOCK_SIZE) + 1;
	sb->inode_size = sizeof(a1fs_inode);

	sb->free_inodes_count = sb->inodes_count - 1;		// reserve inodes_table[0] for root directory inode
	sb->used_dirs_count = 1;		// root directory is in used
	int inode_bitmap_blks_count = (sb->inodes_count % (A1FS_BLOCK_SIZE * 8) == 0)
			? sb->inodes_count / (A1FS_BLOCK_SIZE * 8)
			: (sb->inodes_count / (A1FS_BLOCK_SIZE * 8)) + 1;
	int inode_table_blks_count = (sb->inodes_count * sb->inode_size % A1FS_BLOCK_SIZE == 0)
			? sb->inodes_count * sb->inode_size / A1FS_BLOCK_SIZE
			: (sb->inodes_count * sb->inode_size / A1FS_BLOCK_SIZE) + 1;
	int remaining_blks_count = sb->blocks_count - 1 - inode_bitmap_blks_count - inode_table_blks_count;

			// Check if the image file has room for at least 1 data block
	if (remaining_blks_count <= 1) {
		return false;
	}

	int data_bitmap_blks_count = (remaining_blks_count % (A1FS_BLOCK_SIZE * 8) == 0)
			? remaining_blks_count / (A1FS_BLOCK_SIZE * 8)
			: (remaining_blks_count / (A1FS_BLOCK_SIZE * 8)) + 1;

	sb->free_data_blocks_count = remaining_blks_count - data_bitmap_blks_count;

	sb->inode_bitmap_blk = 1;
	sb->data_bitmap_blk = sb->inode_bitmap_blk + inode_bitmap_blks_count;
	sb->inode_table_blk = sb->data_bitmap_blk + data_bitmap_blks_count;
	sb->first_data_blk = sb->inode_table_blk + inode_table_blks_count;
	sb->data_blocks_count = sb->blocks_count - sb->first_data_blk;


			// Check if the image file is large enough to accommodate Superblock, bitmaps and inode table
	if (size <= (size_t)(1 + inode_bitmap_blks_count + data_bitmap_blks_count + inode_table_blks_count) * A1FS_BLOCK_SIZE) {
		return false;
	}

		// 2. Inode Bitmap
	unsigned char *inode_bitmap = (unsigned char *)(image + A1FS_BLOCK_SIZE * sb->inode_bitmap_blk);
	memset(inode_bitmap, 0, inode_bitmap_blks_count * A1FS_BLOCK_SIZE);
	inode_bitmap[0] = 1 << 7;	// 1000 0000 where 1 is the root inode bit

		// 3. Data Bitmap
	unsigned char *data_bitmap = (unsigned char *)(image + A1FS_BLOCK_SIZE * sb->data_bitmap_blk);
	memset(data_bitmap, 0, data_bitmap_blks_count * A1FS_BLOCK_SIZE);

		// 4. Inode Table
	struct a1fs_inode *inode_table = (struct a1fs_inode *)(image + A1FS_BLOCK_SIZE * sb->inode_table_blk);
	memset(inode_table, 0, inode_table_blks_count * A1FS_BLOCK_SIZE);
	struct a1fs_inode *root_inode_ptr = &inode_table[0];

			// Initialize root directory inode
	root_inode_ptr->mode = S_IFDIR | 0777;
	root_inode_ptr->links = 2;
	root_inode_ptr->size = 0;
	clock_gettime(CLOCK_REALTIME, &(root_inode_ptr->mtime));
	root_inode_ptr->index = 0;
	root_inode_ptr->used_blocks_count = 0;
	root_inode_ptr->extents_blk = -1;		// no extents block for empty root directory
	root_inode_ptr->extents_count = 0;

	return true;
}


int main(int argc, char *argv[])
{
	mkfs_opts opts = {0};// defaults are all 0
	if (!parse_args(argc, argv, &opts)) {
		// Invalid arguments, print help to stderr
		print_help(stderr, argv[0]);
		return 1;
	}
	if (opts.help) {
		// Help requested, print it to stdout
		print_help(stdout, argv[0]);
		return 0;
	}

	// Map image file into memory
	size_t size;
	void *image = map_file(opts.img_path, A1FS_BLOCK_SIZE, &size);
	if (!image) {
		return 1;
	}

	// Check if overwriting existing file system
	int ret = 1;
	if (!opts.force && a1fs_is_present(image)) {
		fprintf(stderr, "Image already contains a1fs; use -f to overwrite\n");
		goto end;
	}

	if (opts.zero) {
		memset(image, 0, size);
	}
	if (!mkfs(image, size, &opts)) {
		fprintf(stderr, "Failed to format the image\n");
		goto end;
	}

	ret = 0;
end:
	munmap(image, size);
	return ret;
}
