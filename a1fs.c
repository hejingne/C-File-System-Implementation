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
 * CSC369 Assignment 1 - a1fs driver implementation.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <libgen.h>

// Using 2.9.x FUSE API
#define FUSE_USE_VERSION 29
#include <fuse.h>

#include "a1fs.h"
#include "fs_ctx.h"
#include "options.h"
#include "map.h"

//NOTE: All path arguments are absolute paths within the a1fs file system and
// start with a '/' that corresponds to the a1fs root directory.
//
// For example, if a1fs is mounted at "~/my_csc369_repo/a1b/mnt/", the path to a
// file at "~/my_csc369_repo/a1b/mnt/dir/file" (as seen by the OS) will be
// passed to FUSE callbacks as "/dir/file".
//
// Paths to directories (except for the root directory - "/") do not end in a
// trailing '/'. For example, "~/my_csc369_repo/a1b/mnt/dir/" will be passed to
// FUSE callbacks as "/dir".


/****************** HELPER FUNCTIONS *********************/

/* FS */
a1fs_inode *get_ino(fs_ctx *fs, int ino_no) {
	a1fs_inode *itable = fs->inode_table;
	return &itable[ino_no];
}

a1fs_extent *get_exts_blk(fs_ctx *fs, a1fs_inode *ino) {
	a1fs_extent *exts_blk = (a1fs_extent *)(fs->first_data_blk + A1FS_BLOCK_SIZE * ino->extents_blk);
	return exts_blk;
}

void *get_db(fs_ctx *fs, int db_no) {
	void *db = fs->first_data_blk + A1FS_BLOCK_SIZE * db_no;
	return db;
}


/* Bitmaps */
int allocate_bit(unsigned char *bitmap, int count) {
	int groupsOfBits = (count % 8 == 0)
			? count / 8
			: (count / 8) + 1;
	int index = 0;

	for (int i = 0; i < groupsOfBits; i++) {
		for (int j = 0; j < 8; j++) {
			unsigned char mask = 1 << (7 - j);
			if (!(bitmap[i] & mask)) {
				bitmap[i] = bitmap[i] | mask;		// switch bit from 0 to 1
				return index;
			}
			index++;
		}
	}

	return -ENOSPC;
}


void allocate_bit_at_index(unsigned char *bitmap, int index) {
	int groupOfBits = index / 8;
	int bitInGroup = index % 8;
	unsigned char mask = 1 << (7 - bitInGroup);
	bitmap[groupOfBits] = bitmap[groupOfBits] | mask;
}


void allocate_contiguous_bits_at_index(unsigned char *bitmap, int index, int num_of_blks) {
	for (int i = 0; i < num_of_blks; i++) {
		int groupOfBits = (index+i) / 8;
		int bitInGroup = (index+i) % 8;
		unsigned char mask = 1 << (7 - bitInGroup);
		bitmap[groupOfBits] = bitmap[groupOfBits] | mask;
	}
}


void deallocate_bit_at_index(unsigned char *bitmap, int index) {
	int groupOfBits = index / 8;
	int bitInGroup = index % 8;
	int mask = ~(1 << (7 - bitInGroup));
	bitmap[groupOfBits] = bitmap[groupOfBits] & mask;		// switch bit from 1 to 0
}


bool check_if_contiguous_dbs_are_free(fs_ctx *fs, int index, int num_of_blks) {
	for (int i = index; i < index + num_of_blks; i++) {
		int groupOfBits = i / 8;
		int bitInGroup = i % 8;
		unsigned char mask = 1 << (7 - bitInGroup);
		unsigned char *d_bitmap = fs->data_bitmap;
		if (d_bitmap[groupOfBits] & mask) {
			return false;
		}
	}
	return true;
}


int find_contiguous_dbs_start_from_index(fs_ctx *fs, int startingIndex, int num_of_blks) {
	a1fs_superblock *sb = fs->sb;
	unsigned char *d_bitmap = fs->data_bitmap;

	if ((int)sb->free_data_blocks_count < num_of_blks) {
		return -ENOSPC;
	}

	int total_dbs = sb->data_blocks_count;
	int groupsOfBits = (total_dbs % 8 == 0)
			? total_dbs / 8
			: (total_dbs / 8) + 1;

	int index = startingIndex;
	int startingGroup = startingIndex / 8;
	int startingBit = startingIndex % 8;
	// Search from bits between startingIndex to end of d_bitmap first
	for (int i = startingGroup; i < groupsOfBits; i++) {
		if (i != startingGroup) {
			startingBit = 0;
		}
		for (int j = startingBit; j < 8; j++) {
			unsigned char mask = 1 << (7 - j);
			if (!(d_bitmap[i] & mask)) {
				if (check_if_contiguous_dbs_are_free(fs, index, num_of_blks)) {
					return index;
				}
			}
			index++;
		}
	}

	index = 0;
	// Search from bits between 0 to startingIndex then
	for (int i = 0; i < startingGroup + 1; i++) {
		if (i == startingGroup) {
			startingBit = startingIndex % 8;
		} else {
			startingBit = 8;
		}
		for (int j = 0; j < startingBit; j++) {
			unsigned char mask = 1 << (7 - j);
			if (!(d_bitmap[i] & mask)) {
				if (check_if_contiguous_dbs_are_free(fs, index, num_of_blks)) {
					return index;
				}
			}
			index++;
		}
	}

	return -1;
}



/* Inode */
int allocate_ino(fs_ctx *fs, mode_t mode, uint32_t links) {
	a1fs_superblock *sb = fs->sb;

	if ((int)sb->free_inodes_count < 1) {
		return -ENOSPC;
	}

	int new_ino_no = allocate_bit(fs->inode_bitmap, sb->inodes_count);
	if (new_ino_no < 0) {
		return new_ino_no;
	} else {
		a1fs_inode *new_ino = get_ino(fs, new_ino_no);

		new_ino->mode = mode;
		new_ino->links = links;
		new_ino->size = 0;
		clock_gettime(CLOCK_REALTIME, &(new_ino->mtime));
		new_ino->index = new_ino_no;
		new_ino->used_blocks_count = 0;
		new_ino->extents_blk = -1;		// no extents block for empty file or dir
		new_ino->extents_count = 0;

		sb->free_inodes_count -= 1;

		return new_ino_no;
	}
}


void deallocate_ino_at_index(fs_ctx *fs, int index) {
	deallocate_bit_at_index(fs->inode_bitmap, index);
	fs->sb->free_inodes_count += 1;
}



/* Data Block */
void initialize_dbs_at_index_for_ino(fs_ctx *fs, a1fs_inode *ino, int db_no, int num_of_blks) {
	allocate_contiguous_bits_at_index(fs->data_bitmap, db_no, num_of_blks);
	ino->used_blocks_count += num_of_blks;
	fs->sb->free_data_blocks_count -= num_of_blks;
	for (int i = 0; i < num_of_blks; i++) {
		memset(get_db(fs, db_no + i), 0, A1FS_BLOCK_SIZE);
	}
}


/**
* Returns blk number for the last data blk allocated for ino.
*	If ino doesn't have any dentries, return its ext blk number.
*/
int get_last_data_blk_no(fs_ctx *fs, a1fs_inode *ino) {
	if (ino->extents_count == 0) {
		return ino->extents_blk;
	} else {
		a1fs_extent *ext_blk = get_exts_blk(fs, ino);
		a1fs_extent last_ext = ext_blk[ino->extents_count - 1];
		return last_ext.start + (last_ext.count - 1);
	}
}


int get_size_in_last_blk(a1fs_inode *ino) {
	if (ino->size == 0) {
		return 0;
	}
	return (ino->size % A1FS_BLOCK_SIZE == 0)
			? A1FS_BLOCK_SIZE
			: ino->size % A1FS_BLOCK_SIZE;
}


void deallocate_db_for_ino(fs_ctx *fs, a1fs_inode *ino, int db_no) {
	deallocate_bit_at_index(fs->data_bitmap, db_no);
	ino->used_blocks_count -= 1;
	fs->sb->free_data_blocks_count += 1;
}


void *get_ptr_to_end_of_file(fs_ctx *fs, a1fs_inode *file_ino) {
	void *ptr_to_file_last_data_blk = get_db(fs, get_last_data_blk_no(fs, file_ino));
	return  ptr_to_file_last_data_blk + (file_ino->size % A1FS_BLOCK_SIZE);
}


void traverse_exts_to_get_byte_ptr(fs_ctx *fs, a1fs_inode *parent_ino, int num_of_blks, int byte_index, void **result) {
	a1fs_extent *exts_blk = get_exts_blk(fs, parent_ino);
	int blks_counter = 0;
	for (int i = 0; i < (int)parent_ino->extents_count; i++) {
		a1fs_extent ext = exts_blk[i];
		for (int j = 0; j < (int)ext.count; j++) {
			blks_counter++;
			if (blks_counter == num_of_blks) {
				void *db = get_db(fs, ext.start + j);
				*result = (byte_index % A1FS_BLOCK_SIZE == 0) 		// e.g. when byte_index == 4096
				 		? db + A1FS_BLOCK_SIZE			// return pointer to file's first db + 4096
						: db + byte_index;
			}
		}
	}
}

void *get_ptr_to_byte_in_file(fs_ctx *fs, a1fs_inode *file_ino, int byte_index){
	if (byte_index == 0) {
		a1fs_extent *exts_blk = get_exts_blk(fs, file_ino);
		a1fs_extent first_ext = exts_blk[0];
		return get_db(fs, first_ext.start);
	}
	// db_number_in_file is the number of data blks belong to file that we need to scan to find the byte at index {byte_index}
	int db_number_in_file = byte_index / A1FS_BLOCK_SIZE + ((byte_index % A1FS_BLOCK_SIZE) != 0);
	void *result;
	traverse_exts_to_get_byte_ptr(fs, file_ino, db_number_in_file, byte_index, &result);
	return result;
}



/* Extent */
int initialize_ext_blk_for_ino(fs_ctx *fs, a1fs_inode *ino) {
	a1fs_superblock *sb = fs->sb;

	if ((int)sb->free_data_blocks_count < 1) {
		return -ENOSPC;
	}
	ino->extents_blk = allocate_bit(fs->data_bitmap, sb->data_blocks_count);
	ino->used_blocks_count += 1;
	sb->free_data_blocks_count -= 1;
	memset(get_exts_blk(fs, ino), 0, A1FS_BLOCK_SIZE);
	return 0;
}


void add_to_ext_blk_for_ino(fs_ctx *fs, a1fs_inode *ino, int data_blk_no, int num_of_blks) {
	a1fs_extent *ext_blk = get_exts_blk(fs, ino);
	a1fs_extent *new_ext = &ext_blk[ino->extents_count];
	new_ext->start = data_blk_no;
	new_ext->count = num_of_blks;

	ino->extents_count += 1;
}


void shrink_ext_for_ino(fs_ctx *fs, a1fs_inode *ino) {
	a1fs_extent *ext_blk = get_exts_blk(fs, ino);
	a1fs_extent *last_ext = &ext_blk[ino->extents_count - 1];
	if (last_ext->count == 1) {
		ino->extents_count -= 1;
	} else {
		last_ext->count -= 1;
	}
	if (ino->extents_count == 0) {
		deallocate_db_for_ino(fs, ino, ino->extents_blk);
		ino->extents_blk = -1;
	}
}



/* Directory Entry */
void add_to_dentries_blk_for_ino(fs_ctx *fs, a1fs_inode *parent_ino, int dentries_blk_no, int dentry_ino_no, char *dentry_name) {
	a1fs_dentry *dentries_blk = (a1fs_dentry *) get_db(fs, dentries_blk_no);

	a1fs_dentry *new_entry = &dentries_blk[parent_ino->size / sizeof(a1fs_dentry)];
	new_entry->ino = dentry_ino_no;
	strncpy(new_entry->name, dentry_name, A1FS_NAME_MAX);

	parent_ino->size += sizeof(a1fs_dentry);
}


a1fs_dentry *get_last_dentry_for_ino(fs_ctx *fs, a1fs_inode *parent_ino) {
	a1fs_dentry *last_entries_blk = (a1fs_dentry *) get_db(fs, get_last_data_blk_no(fs, parent_ino));
	int num_of_entries_in_last_blk = get_size_in_last_blk(parent_ino) / sizeof(a1fs_dentry);
	return &last_entries_blk[num_of_entries_in_last_blk - 1];
}




/* File */
int extend_file(fs_ctx *fs, a1fs_inode *file_ino, int additional_bytes) {
	if (additional_bytes == 0) { return 0; }

	// 1. Initialize ext blk for file inode if needed
	if (file_ino->extents_blk == -1) {
		if (initialize_ext_blk_for_ino(fs, file_ino) < 0) {	return -ENOSPC; }
	}

	// 2. Write to file's data blks
	int add_bytes = 0;
	while (additional_bytes != 0) {
			if (file_ino->size % A1FS_BLOCK_SIZE == 0) {	// Means new db is needed to store the bytes

				int num_of_blks_to_allocate = (additional_bytes % A1FS_BLOCK_SIZE == 0)
								? additional_bytes / A1FS_BLOCK_SIZE
								: (additional_bytes / A1FS_BLOCK_SIZE) + 1;
				// find the next available dbs following file_ino's last db to reduce file fragmentation
				int last_db_no = get_last_data_blk_no(fs, file_ino);
				int new_db_no = find_contiguous_dbs_start_from_index(fs, last_db_no + 1, num_of_blks_to_allocate);
				if (new_db_no == -ENOSPC) {
					 return -ENOSPC;
				} else if (new_db_no == -1) {
					num_of_blks_to_allocate = 1;
					new_db_no = find_contiguous_dbs_start_from_index(fs, last_db_no + 1, 1);
				}
				if (A1FS_EXTS_MAX < (int)(file_ino->extents_count + num_of_blks_to_allocate)) {
					return -ENOSPC;
				}

				initialize_dbs_at_index_for_ino(fs, file_ino, new_db_no, num_of_blks_to_allocate);	// 4.1. Allocate new dbs
				add_bytes = (num_of_blks_to_allocate * A1FS_BLOCK_SIZE >= additional_bytes)
							? additional_bytes
							: num_of_blks_to_allocate * A1FS_BLOCK_SIZE;
				add_to_ext_blk_for_ino(fs, file_ino, new_db_no, num_of_blks_to_allocate);	// 4.2. Add extent

			} else {	// Means we should fill up file's last db first
				int leftover_bytes_in_last_blk = A1FS_BLOCK_SIZE - (file_ino->size % A1FS_BLOCK_SIZE);
				add_bytes = (leftover_bytes_in_last_blk >= additional_bytes)
							? additional_bytes
							: leftover_bytes_in_last_blk;
			}
		// 4.3. Update file size
		file_ino->size += add_bytes;
		additional_bytes -= add_bytes;
	}

	// 5. Update metadata p.s. some metadata has been updated by helper functions
	clock_gettime(CLOCK_REALTIME, &(file_ino->mtime));

	return 0;
}



int shrink_file(fs_ctx *fs, a1fs_inode *file_ino, int unwanted_bytes) {
	if (file_ino->extents_blk == -1 || file_ino->extents_count == 0 || file_ino->size == 0) { return -ENOSPC; }

	while (unwanted_bytes != 0) {
		int bytes_in_last_blk = get_size_in_last_blk(file_ino);
		int	bytes_to_remove_in_last_blk = (bytes_in_last_blk > unwanted_bytes)
				? unwanted_bytes
				: bytes_in_last_blk;
		file_ino->size -= bytes_to_remove_in_last_blk;
		unwanted_bytes -= bytes_to_remove_in_last_blk;
		if (file_ino->size % A1FS_BLOCK_SIZE == 0) {	// Means file's last db is emptied
			deallocate_db_for_ino(fs, file_ino, get_last_data_blk_no(fs, file_ino));
			shrink_ext_for_ino(fs, file_ino);
		}
	}
	clock_gettime(CLOCK_REALTIME, &(file_ino->mtime));
	return 0;
}



/* Directory Entries Traversal */
int traverse_exts_to_fill_name(fs_ctx *fs, a1fs_inode *parent_ino, void *buf, fuse_fill_dir_t filler) {
	if (parent_ino->extents_blk == -1) { return 0; }	// do nothing when no ext blk has been allocated for this dir

	int dentries_total = parent_ino->size / sizeof(a1fs_dentry);
	a1fs_extent *exts_blk = get_exts_blk(fs, parent_ino);

  for (int i = 0; i < (int)parent_ino->extents_count; i++) {
    a1fs_extent ext = exts_blk[i];

		for (int j = 0; j < (int)ext.count; j++) {
			struct a1fs_dentry *entries_blk = (struct a1fs_dentry *) get_db(fs, ext.start + j);

			int dentries_in_this_blk;
			if (dentries_total > A1FS_EXT_DENTRIES_MAX) {
				dentries_in_this_blk = A1FS_EXT_DENTRIES_MAX;
				dentries_total -= A1FS_EXT_DENTRIES_MAX;
			} else {
				dentries_in_this_blk = dentries_total;
			}

			for (int k = 0; k < dentries_in_this_blk; k++) {
				struct a1fs_dentry entry = entries_blk[k];
				if (filler(buf, entry.name, NULL, 0) != 0) { return -ENOMEM; }
			}
		}
  }

	return 0;
}


int traverse_exts_to_replace_dentry(fs_ctx *fs, a1fs_inode *parent_ino, char *dentry_name) {
	if (parent_ino->size == 0) { return 0; }
	int dentries_total = parent_ino->size / sizeof(a1fs_dentry);
	a1fs_extent *exts_blk = get_exts_blk(fs, parent_ino);

  for (int i = 0; i < (int)parent_ino->extents_count; i++) {
    a1fs_extent ext = exts_blk[i];

		for (int j = 0; j < (int)ext.count; j++) {
			struct a1fs_dentry *entries_blk = (struct a1fs_dentry *) get_db(fs, ext.start + j);

			int dentries_in_this_blk;
			if (dentries_total > A1FS_EXT_DENTRIES_MAX) {
				dentries_in_this_blk = A1FS_EXT_DENTRIES_MAX;
				dentries_total -= A1FS_EXT_DENTRIES_MAX;
			} else {
				dentries_in_this_blk = dentries_total;
			}

			for (int k = 0; k < dentries_in_this_blk; k++) {
				struct a1fs_dentry *entry = &entries_blk[k];
				if (strcmp(entry->name, dentry_name) == 0) {
					 a1fs_dentry *last_entry = get_last_dentry_for_ino(fs, parent_ino);
					 entry->ino = last_entry->ino;
					 strncpy(entry->name, last_entry->name, A1FS_NAME_MAX);
					 break;
				}
			}
		}
  }
	parent_ino->size -= sizeof(a1fs_dentry);
	return 0;
}


int traverse_exts_to_deallocate_dbs(fs_ctx *fs, a1fs_inode *parent_ino) {
	if (parent_ino->extents_blk == -1) { return 0; }	// do nothing when no ext blk has been allocated for this dir

	a1fs_extent *exts_blk = get_exts_blk(fs, parent_ino);
  for (int i = 0; i < (int)parent_ino->extents_count; i++) {
    a1fs_extent ext = exts_blk[i];
		for (int j = 0; j < (int)ext.count; j++) {
			deallocate_db_for_ino(fs, parent_ino, ext.start + j);
		}
  }

	deallocate_db_for_ino(fs, parent_ino, parent_ino->extents_blk);
	return 0;
}




int get_dentry_ino_no(fs_ctx *fs, a1fs_inode *parent_ino, char *dentry_name) {
	if (parent_ino->extents_blk == -1) {
		return -ENOENT;
	}
	if (!S_ISDIR(parent_ino->mode)) {
		return -ENOTDIR;
	}

	int dentries_total = parent_ino->size / sizeof(a1fs_dentry);;
	a1fs_extent *exts_blk = get_exts_blk(fs, parent_ino);

  for (int i = 0; i < (int)parent_ino->extents_count; i++) {	// Traverse all extents
		a1fs_extent ext = exts_blk[i];

		for (int j = 0; j < (int)ext.count; j++) {	// Traverse all data blks in one extent
			a1fs_dentry *entries_blk = (a1fs_dentry *) get_db(fs, ext.start + j);

			int dentries_in_this_blk;
			if (dentries_total > A1FS_EXT_DENTRIES_MAX) {
				dentries_in_this_blk = A1FS_EXT_DENTRIES_MAX;
				dentries_total -= A1FS_EXT_DENTRIES_MAX;
			} else {
				dentries_in_this_blk = dentries_total;
			}

			for (int k = 0; k < dentries_in_this_blk; k++) {	// Traverse all entries in one data blk
				a1fs_dentry entry = entries_blk[k];
				if (strcmp(entry.name, dentry_name) == 0) {
		      return entry.ino;
		    }
			}
		}
  }

  return -ENOENT;
}



/* Path */
int path_lookup(fs_ctx *fs, const char *path, bool look_for_parent) {
    char buffer[A1FS_PATH_MAX];
    char *copy = strncpy(buffer, path, A1FS_PATH_MAX);
    char *search_name;
    int parent_ino_no = 0;
		int child_ino_no = 0;

    while ( (search_name = strsep(&copy, "/")) != NULL ) {
			if (strcmp(search_name, "") == 0) {
				continue;
			}
			parent_ino_no = child_ino_no;
			a1fs_inode *parent_ino = get_ino(fs, parent_ino_no);
      child_ino_no = get_dentry_ino_no(fs, parent_ino, search_name);
      if (child_ino_no < 0) {
        break;
      }
    }

    return look_for_parent ? parent_ino_no : child_ino_no;
}

void path_lookup_for_last_dentry(const char *path, char last_dentry_name[A1FS_NAME_MAX]) {
		char buffer[A1FS_PATH_MAX];
		strcpy(buffer, path);
		strncpy(last_dentry_name, basename(buffer), A1FS_NAME_MAX);
}


/** Get file system context. */
static fs_ctx *get_fs(void)
{
	return (fs_ctx*)fuse_get_context()->private_data;
}


/**
 * Initialize the file system.
 *
 * Called when the file system is mounted. NOTE: we are not using the FUSE
 * init() callback since it doesn't support returning errors. This function must
 * be called explicitly before fuse_main().
 *
 * @param fs    file system context to initialize.
 * @param opts  command line options.
 * @return      true on success; false on failure.
 */
static bool a1fs_init(fs_ctx *fs, a1fs_opts *opts)
{
	// Nothing to initialize if only printing help
	if (opts->help) {
		return true;
	}

	size_t size;
	void *image = map_file(opts->img_path, A1FS_BLOCK_SIZE, &size);
	if (!image) {
		return false;
	}

	return fs_ctx_init(fs, image, size);
}

/**
 * Cleanup the file system.
 *
 * Called when the file system is unmounted. Must cleanup all the resources
 * created in a1fs_init().
 */
static void a1fs_destroy(void *ctx)
{
	fs_ctx *fs = (fs_ctx*)ctx;
	if (fs->image) {
		munmap(fs->image, fs->size);
		fs_ctx_destroy(fs);
	}
}


/**
 * Get file system statistics.
 *
 * Implements the statvfs() system call. See "man 2 statvfs" for details.
 * The f_bfree and f_bavail fields should be set to the same value.
 * The f_ffree and f_favail fields should be set to the same value.
 * The following fields can be ignored: f_fsid, f_flag.
 * All remaining fields are required.
 *
 * Errors: none
 *
 * @param path  path to any file in the file system. Can be ignored.
 * @param st    pointer to the struct statvfs that receives the result.
 * @return      0 on success; -errno on error.
 */
static int a1fs_statfs(const char *path, struct statvfs *st)
{
	(void)path;// unused
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));
	st->f_bsize   = A1FS_BLOCK_SIZE;
	st->f_frsize  = A1FS_BLOCK_SIZE;
	//TODO: fill in the rest of required fields based on the information stored
	// in the superblock
	a1fs_superblock *sb = fs->sb;

	st->f_blocks = sb->blocks_count;
	st->f_bfree = sb->free_data_blocks_count;
	st->f_bavail = st->f_bfree;
	st->f_files = sb->inodes_count;
	st->f_ffree = sb->free_inodes_count;
	st->f_favail = st->f_ffree;
	st->f_namemax = A1FS_NAME_MAX;

	return 0;
}


/**
 * Get file or directory attributes.
 *
 * Implements the lstat() system call. See "man 2 lstat" for details.
 * The following fields can be ignored: st_dev, st_ino, st_uid, st_gid, st_rdev,
 *                                      st_blksize, st_atim, st_ctim.
 * All remaining fields are required.
 *
 * NOTE: the st_blocks field is measured in 512-byte units (disk sectors);
 *       it should include any metadata blocks that are allocated to the inode.
 *
 * NOTE: the st_mode field must be set correctly for files and directories.
 *
 * Errors:
 *   ENAMETOOLONG  the path or one of its components is too long.
 *   ENOENT        a component of the path does not exist.
 *   ENOTDIR       a component of the path prefix is not a directory.
 *
 * @param path  path to a file or directory.
 * @param st    pointer to the struct stat that receives the result.
 * @return      0 on success; -errno on error;
 */
static int a1fs_getattr(const char *path, struct stat *st)
{
	if (strlen(path) >= A1FS_PATH_MAX) {
		return -ENAMETOOLONG;
	}

	memset(st, 0, sizeof(*st));
	fs_ctx *fs = get_fs();

	//TODO: lookup the inode for given path and, if it exists, fill in the
	// required fields based on the information stored in the inode
		//NOTE: all the fields set below are required and must be set according
		// to the information stored in the corresponding inode
	int ino_no = path_lookup(fs, path, false);
	if (ino_no < 0) {
		return ino_no;
	} else {
		a1fs_inode *inode = get_ino(fs, ino_no);

		st->st_mode = inode->mode;
		st->st_nlink = inode->links;
		st->st_size = inode->size;
		st->st_blocks = inode->used_blocks_count * A1FS_BLOCK_SIZE / 512;
		st->st_mtim = inode->mtime;

		return 0;
	}
}


/**
 * Read a directory.
 *
 * Implements the readdir() system call. Should call filler(buf, name, NULL, 0)
 * for each directory entry. See fuse.h in libfuse source code for details.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a filler() call failed).
 *
 * @param path    path to the directory.
 * @param buf     buffer that receives the result.
 * @param filler  function that needs to be called for each directory entry.
 *                Pass 0 as offset (4th argument). 3rd argument can be NULL.
 * @param offset  unused.
 * @param fi      unused.
 * @return        0 on success; -errno on error.
 */
static int a1fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
	(void)offset;// unused
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//TODO: lookup the directory inode for given path and iterate through its
	// directory entries
	if (filler(buf, "." , NULL, 0) != 0) { return -ENOMEM; }
	if (filler(buf, "..", NULL, 0) != 0) { return -ENOMEM; }

	int ino_no = path_lookup(fs, path, false);
	a1fs_inode *ino = get_ino(fs, ino_no);
	return traverse_exts_to_fill_name(fs, ino, buf, filler);
}


/**
 * Create a directory.
 *
 * Implements the mkdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the directory to create.
 * @param mode  file mode bits.
 * @return      0 on success; -errno on error.
 */
static int a1fs_mkdir(const char *path, mode_t mode)
{
	mode = mode | S_IFDIR;
	fs_ctx *fs = get_fs();

	//TODO: create a directory at given path with given mode

	// 1. Initialize inode for the new directory
	int dir_ino_no = allocate_ino(fs, mode, 2);
	if (dir_ino_no < 0) {	return dir_ino_no; }

	// 2. Get parent directory inode and new directory name
	int parent_ino_no = path_lookup(fs, path, true);
	a1fs_inode *parent_ino = get_ino(fs, parent_ino_no);
	char dir_name[A1FS_NAME_MAX];
	path_lookup_for_last_dentry(path, dir_name);

	// 3. Initialize ext blk for parent dir inode if needed
	if (parent_ino->extents_blk == -1) {
		if (initialize_ext_blk_for_ino(fs, parent_ino) < 0) {	return -ENOSPC; }
	}

	// 4. Add new dentry to parent dir inode
	int last_db_no = get_last_data_blk_no(fs, parent_ino);
	if (parent_ino->size % A1FS_BLOCK_SIZE == 0) {	// Means new db is needed to store the dentry

			// find the next available db following parent_ino's last db to reduce file fragmentation
		int new_db_no = find_contiguous_dbs_start_from_index(fs, last_db_no + 1, 1);
		if (new_db_no == -ENOSPC) { return -ENOSPC; }
		initialize_dbs_at_index_for_ino(fs, parent_ino, new_db_no, 1);	// 4.1. Allocate new db
		add_to_dentries_blk_for_ino(fs, parent_ino, new_db_no, dir_ino_no, dir_name);	// 4.2. Add dentry
		add_to_ext_blk_for_ino(fs, parent_ino, new_db_no, 1);	// 4.3. Add extent

	} else {	// Means the dentry can add to parent_ino's last dentries blk
		add_to_dentries_blk_for_ino(fs, parent_ino, last_db_no, dir_ino_no, dir_name);	// 4.1. Only need to add dentry
	}

	// 5. Update metadata p.s. some metadata has been updated by helper functions
	parent_ino->links += 1;
	clock_gettime(CLOCK_REALTIME, &(parent_ino->mtime));
	fs->sb->used_dirs_count += 1;

	return 0;
}


/**
 * Remove a directory.
 *
 * Implements the rmdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a non-root directory.
 *
 * Errors:
 *   ENOTEMPTY  the directory is not empty.
 *
 * @param path  path to the directory to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rmdir(const char *path)
{
	assert(strcmp(path, "/") != 0);
	fs_ctx *fs = get_fs();

	//TODO: remove the directory at given path (only if it's empty)

	int dir_ino_no = path_lookup(fs, path, false);
	a1fs_inode *dir_ino = get_ino(fs, dir_ino_no); 	// directory inode to be removed
	int parent_ino_no = path_lookup(fs, path, true);
	a1fs_inode *parent_ino = get_ino(fs, parent_ino_no);	// parent directory inode
	char dir_name[A1FS_NAME_MAX];
	path_lookup_for_last_dentry(path, dir_name);

	if (dir_ino->size != 0) {
		return -ENOTEMPTY;
	}

	// 1. Deallocaste all data blks related to the dir inode
	traverse_exts_to_deallocate_dbs(fs, dir_ino);

	// 2. Deallocate its inode
	deallocate_ino_at_index(fs, dir_ino_no);

	// 3. Remove dentry from parent dir inode
	if (parent_ino->size % A1FS_BLOCK_SIZE == sizeof(a1fs_dentry)) {	// Means the last db needs to be deallocated after the removal

		traverse_exts_to_replace_dentry(fs, parent_ino, dir_name);	// 3.1. Replace dentry with the last dentry
		deallocate_db_for_ino(fs, parent_ino, get_last_data_blk_no(fs, parent_ino));	// 3.2. Deallocate last db
		shrink_ext_for_ino(fs, parent_ino);	// 3.3. Shrink extent

	} else {	// Means the last db doesn't need to be deallocated
		traverse_exts_to_replace_dentry(fs, parent_ino, dir_name);	// 3.1. Only need to replace dentry with the last dentry
	}

	// 4. Update metadata p.s. some metadata has been updated by helper functions
	parent_ino->links -= 1;
	clock_gettime(CLOCK_REALTIME, &(parent_ino->mtime));
	fs->sb->used_dirs_count -= 1;

	return 0;
}


/**
 * Create a file.
 *
 * Implements the open()/creat() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to create.
 * @param mode  file mode bits.
 * @param fi    unused.
 * @return      0 on success; -errno on error.
 */
static int a1fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi;// unused
	assert(S_ISREG(mode));
	fs_ctx *fs = get_fs();

	//TODO: create a file at given path with given mode
	// 1. Initialize inode for the new file
	int file_ino_no = allocate_ino(fs, mode, 1);
	if (file_ino_no < 0) { return file_ino_no; }

	// 2. Get parent directory inode and new file name
	int parent_ino_no = path_lookup(fs, path, true);
	a1fs_inode *parent_ino = get_ino(fs, parent_ino_no);
	char dir_name[A1FS_NAME_MAX];
	path_lookup_for_last_dentry(path, dir_name);

	// 3. Initialize ext blk for parent dir inode if needed
	if (parent_ino->extents_blk == -1) {
		if (initialize_ext_blk_for_ino(fs, parent_ino) < 0) {	return -ENOSPC; }
	}

	// 4. Add new dentry to parent dir inode
	int last_db_no = get_last_data_blk_no(fs, parent_ino);
	if (parent_ino->size % A1FS_BLOCK_SIZE == 0) {	// Means new db is needed to store the dentry

		// find the next available db following parent_ino's last db to reduce file fragmentation
		int new_db_no = find_contiguous_dbs_start_from_index(fs, last_db_no + 1, 1);
		if (new_db_no == -ENOSPC) { return -ENOSPC; }
		initialize_dbs_at_index_for_ino(fs, parent_ino, new_db_no, 1);	// 4.1. Allocate new db
		add_to_dentries_blk_for_ino(fs, parent_ino, new_db_no, file_ino_no, dir_name);	// 4.2. Add dentry
		add_to_ext_blk_for_ino(fs, parent_ino, new_db_no, 1);	// 4.3. Add extent

	} else {	// Means the dentry can add to parent_ino's last dentries blk
		add_to_dentries_blk_for_ino(fs, parent_ino, last_db_no, file_ino_no, dir_name);	// 4.1. Only need to add dentry
	}

	// 5. Update metadata p.s. some metadata has been updated by helper functions
	clock_gettime(CLOCK_REALTIME, &(parent_ino->mtime));

	return 0;
}


/**
 * Remove a file.
 *
 * Implements the unlink() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors: none
 *
 * @param path  path to the file to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_unlink(const char *path)
{
	fs_ctx *fs = get_fs();

	//TODO: remove the file at given path
	int file_ino_no = path_lookup(fs, path, false);
	a1fs_inode *file_ino = get_ino(fs, file_ino_no);	// inode for file to be removed
	int parent_ino_no = path_lookup(fs, path, true);
	a1fs_inode *parent_ino = get_ino(fs, parent_ino_no);	// parent directory inode
	char file_name[A1FS_NAME_MAX];
	path_lookup_for_last_dentry(path, file_name);

	// 1. Deallocaste all data blks related to the file inode
	traverse_exts_to_deallocate_dbs(fs, file_ino);

	// 2. Deallocate its inode
	deallocate_ino_at_index(fs, file_ino_no);

	// 3. Remove dentry from parent dir inode
	if (parent_ino->size % A1FS_BLOCK_SIZE == sizeof(a1fs_dentry)) {	// Means the last db needs to be deallocated after the removal

		traverse_exts_to_replace_dentry(fs, parent_ino, file_name);	// 3.1. Replace dentry with the last dentry
		deallocate_db_for_ino(fs, parent_ino, get_last_data_blk_no(fs, parent_ino));	// 3.2. Deallocate last db
		shrink_ext_for_ino(fs, parent_ino);	// 3.3. Shrink extent

	} else {	// Means the last db doesn't need to be deallocated
		traverse_exts_to_replace_dentry(fs, parent_ino, file_name);	// 3.1. Only need to replace dentry with the last dentry
	}

	// 4. Update metadata p.s. some metadata has been updated by helper functions
	clock_gettime(CLOCK_REALTIME, &(parent_ino->mtime));

	return 0;
}


/**
 * Change the modification time of a file or directory.
 *
 * Implements the utimensat() system call. See "man 2 utimensat" for details.
 *
 * NOTE: You only need to implement the setting of modification time (mtime).
 *       Timestamp modifications are not recursive.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists.
 *
 * Errors: none
 *
 * @param path   path to the file or directory.
 * @param times  timestamps array. See "man 2 utimensat" for details.
 * @return       0 on success; -errno on failure.
 */
static int a1fs_utimens(const char *path, const struct timespec times[2])
{
	fs_ctx *fs = get_fs();

	//TODO: update the modification timestamp (mtime) in the inode for given
	// path with either the time passed as argument or the current time,
	// according to the utimensat man page

	int ino_no = path_lookup(fs, path, false);
	a1fs_inode *inode_table = fs->inode_table;
	a1fs_inode *inode = &inode_table[ino_no];

	if(times[1].tv_nsec == UTIME_NOW) {
		clock_gettime(CLOCK_REALTIME, &(inode->mtime));
	} else {
		inode->mtime = times[1];
	}

	return 0;
}


/**
 * Change the size of a file.
 *
 * Implements the truncate() system call. Supports both extending and shrinking.
 * If the file is extended, the new uninitialized range at the end must be
 * filled with zeros.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to set the size.
 * @param size  new file size in bytes.
 * @return      0 on success; -errno on error.
 */
static int a1fs_truncate(const char *path, off_t size)
{
	fs_ctx *fs = get_fs();

	//TODO: set new file size, possibly "zeroing out" the uninitialized range
	int ino_no = path_lookup(fs, path, false);
	a1fs_inode *file_ino = get_ino(fs, ino_no);

	int additional_bytes = size - file_ino->size;
	return (additional_bytes >= 0)
		? extend_file(fs, file_ino, additional_bytes)
		: shrink_file(fs, file_ino, additional_bytes*(-1));
}


/**
 * Read data from a file.
 *
 * Implements the pread() system call. Must return exactly the number of bytes
 * requested except on EOF (end of file). Reads from file ranges that have not
 * been written to must return ranges filled with zeros. You can assume that the
 * byte range from offset to offset + size is contained within a single block.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors: none
 *
 * @param path    path to the file to read from.
 * @param buf     pointer to the buffer that receives the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to read from.
 * @param fi      unused.
 * @return        number of bytes read on success; 0 if offset is beyond EOF;
 *                -errno on error.
 */
static int a1fs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//TODO: read data from the file at given offset into the buffer
	a1fs_inode *file_ino = get_ino(fs, path_lookup(fs, path, false));

	if (offset > (int)file_ino->size || file_ino->size == 0 ||
	 		file_ino->extents_blk == -1 || file_ino->extents_count == 0) {
		return 0;
	}

	void *ptr_to_eof = get_ptr_to_end_of_file(fs, file_ino);
	void *ptr_to_offset = get_ptr_to_byte_in_file(fs, file_ino, offset);
	int readable_size = ptr_to_eof - ptr_to_offset;

	if (readable_size > (int)size) {
		memcpy(buf, ptr_to_offset, size);
		return size;
	} else {
		memcpy(buf, ptr_to_offset, readable_size);
		return readable_size;
	}
}

/**
 * Write data to a file.
 *
 * Implements the pwrite() system call. Must return exactly the number of bytes
 * requested except on error. If the offset is beyond EOF (end of file), the
 * file must be extended. If the write creates a "hole" of uninitialized data,
 * the new uninitialized range must filled with zeros. You can assume that the
 * byte range from offset to offset + size is contained within a single block.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *   ENOSPC  too many extents (a1fs only needs to support 512 extents per file)
 *
 * @param path    path to the file to write to.
 * @param buf     pointer to the buffer containing the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to write to.
 * @param fi      unused.
 * @return        number of bytes written on success; -errno on error.
 */
static int a1fs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//TODO: write data from the buffer into the file at given offset, possibly
	// "zeroing out" the uninitialized range
	a1fs_inode *file_ino = get_ino(fs, path_lookup(fs, path, false));

	int uninitialized_bytes = offset - file_ino->size;
	if (extend_file(fs, file_ino, uninitialized_bytes) < 0) { return -ENOSPC;}
	void *ptr_to_eof_before_write = get_ptr_to_end_of_file(fs, file_ino);

	if (extend_file(fs, file_ino, size) < 0) { return -ENOSPC;}
	memcpy(ptr_to_eof_before_write, buf, size);

	clock_gettime(CLOCK_REALTIME, &(file_ino->mtime));
	return size;
}


static struct fuse_operations a1fs_ops = {
	.destroy  = a1fs_destroy,
	.statfs   = a1fs_statfs,
	.getattr  = a1fs_getattr,
	.readdir  = a1fs_readdir,
	.mkdir    = a1fs_mkdir,
	.rmdir    = a1fs_rmdir,
	.create   = a1fs_create,
	.unlink   = a1fs_unlink,
	.utimens  = a1fs_utimens,
	.truncate = a1fs_truncate,
	.read     = a1fs_read,
	.write    = a1fs_write,
};

int main(int argc, char *argv[])
{
	a1fs_opts opts = {0};// defaults are all 0
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (!a1fs_opt_parse(&args, &opts)) {
		return 1;
	}

	fs_ctx fs = {0};
	if (!a1fs_init(&fs, &opts)) {
		fprintf(stderr, "Failed to mount the file system\n");
		return 1;
	}

	return fuse_main(args.argc, args.argv, &a1fs_ops, &fs);
}
