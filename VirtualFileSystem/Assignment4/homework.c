/*
 * homework.c
 *
 *  Created on: Aug 6, 2019
 */

#define FUSE_USE_VERSION 27

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <limits.h>
#include <fuse.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <assert.h>

#include "fsx600.h"
#include "blkdev.h"

//extern int homework_part;       /* set by '-part n' command-line option */

typedef struct fs_inode Inode;
typedef struct fs_dirent DirEntry;
typedef struct fs_super FSuperBlock;

FSuperBlock* sbPtr;
struct fs_super sb;
/*
 * disk access - the global variable 'disk' points to a blkdev
 * structure which has been initialized to access the image file.
 *
 * NOTE - blkdev access is in terms of 1024-byte blocks
 */
extern struct blkdev *disk;

/* by defining bitmaps as 'fd_set' pointers, you can use existing
 * macros to handle them.
 *   FD_ISSET(##, inode_map);
 *   FD_CLR(##, block_map);
 *   FD_SET(##, block_map);
 */

/** pointer to inode bitmap to determine free inodes */
static fd_set *inode_map;
static int     inode_map_base;

/** pointer to inode blocks */
static struct fs_inode *inodes;
/** number of inodes from superblock */
static int   n_inodes;
/** number of first inode block */
static int   inode_base;

/** pointer to block bitmap to determine free blocks */
fd_set *block_map;
/** number of first data block */
static int     block_map_base;

/** number of available blocks from superblock */
static int   n_blocks;

/** number of root inode from superblock */
static int   root_inode;

/** array of dirty metadata blocks to write  -- optional */
static void **dirty;

/** length of dirty array -- optional */
static int    dirty_len;


#include "helper.h"


/* Fuse functions
 */

/**
 * init - this is called once by the FUSE framework at startup.
 *
 * This is a good place to read in the super-block and set up any
 * global variables you need. You don't need to worry about the
 * argument or the return value.
 *
 * @param conn fuse connection information - unused
 * @return unused - returns NULL
 */
void* fs_init(struct fuse_conn_info *conn)
{
	// read the superblock
    sbPtr = &sb;
    if (disk->ops->read(disk, 0, 1, &sb) < 0) {
        exit(1);
    }

    root_inode = sb.root_inode;

    /* The inode map and block map are written directly to the disk after the superblock */

    // read inode map
    inode_map_base = 1;
    inode_map = malloc(sb.inode_map_sz * FS_BLOCK_SIZE);
    if (disk->ops->read(disk, inode_map_base, sb.inode_map_sz, inode_map) < 0) {
        exit(1);
    }

    // read block map
    block_map_base = inode_map_base + sb.inode_map_sz;
    block_map = malloc(sb.block_map_sz * FS_BLOCK_SIZE);
    if (disk->ops->read(disk, block_map_base, sb.block_map_sz, block_map) < 0) {
        exit(1);
    }

    /* The inode data is written to the next set of blocks */
    inode_base = block_map_base + sb.block_map_sz;
    n_inodes = sb.inode_region_sz * INODES_PER_BLK;
    inodes = malloc(sb.inode_region_sz * FS_BLOCK_SIZE); //read innodes to memory
    if (disk->ops->read(disk, inode_base, sb.inode_region_sz, inodes) < 0) {
        exit(1);
    }

    // number of blocks on device
    n_blocks = sb.num_blocks;

    // dirty metadata blocks
    dirty_len = inode_base + sb.inode_region_sz;
    dirty = calloc(dirty_len*sizeof(void*), 1);
    return NULL;
}

/* Note on path translation errors:
 * In addition to the method-specific errors listed below, almost
 * every method can return one of the following errors if it fails to
 * locate a file or directory corresponding to a specified path.
 *
 * ENOENT - a component of the path is not present.
 * ENOTDIR - an intermediate component of the path (e.g. 'b' in
 *           /a/b/c) is not a directory $todo
 */

/* note on splitting the 'path' variable:
 * the value passed in by the FUSE framework is declared as 'const',
 * which means you can't modify it. The standard mechanisms for
 * splitting strings in C (strtok, strsep) modify the string in place,
 * so you have to copy the string and then free the copy when you're
 * done. One way of doing this:
 *
 *    char *_path = strdup(path);
 *    int inum = translate(_path); $comment 将path翻译成inode节点
 *    free(_path);
 */

/**
 * getattr - get file or directory attributes. For a description of
 * the fields in 'struct stat', see 'man lstat'.
 *
 * Note - fields not provided in CS5600fs are:
 *    st_nlink - always set to 1
 *    st_atime, st_ctime - set to same value as st_mtime
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @param sb pointer to stat struct
 * @return 0 if successful, or -error number
 */
static int fs_getattr(const char *path, struct stat *sb)
{   
    uint8_t is_real_dir;
    int dir_inode_index = translate(path, &is_real_dir);
    if (dir_inode_index < 0) {
        return dir_inode_index;
    }
    Inode tmp_inode = inodes[dir_inode_index];
    sb -> st_ino = dir_inode_index;
    sb -> st_mode = tmp_inode.mode;
    /* number of hard links to the file */
    sb -> st_nlink = 1;
    sb -> st_uid = tmp_inode.uid;
    sb -> st_gid = tmp_inode.gid;
    sb -> st_size = tmp_inode.size;
    (sb -> st_mtimespec).tv_sec = tmp_inode.mtime;
    (sb -> st_ctimespec).tv_sec = tmp_inode.ctime;
    return 0;
}

/**
 * readdir - get directory contents.
 *
 * For each entry in the directory, invoke the 'filler' function,
 * which is passed as a function pointer, as follows:
 *     filler(buf, <name>, <statbuf>, 0)
 * where <statbuf> is a struct stat, just like in getattr.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the directory path
 * @param ptr  filler buf pointer
 * @param filler filler function to call for each entry
 * @param offset the file offset -- unused
 * @param fi the fuse file information
 * @return 0 if successful, or -error number
 */
static int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
    uint8_t is_real_dir;
    struct stat st;
    DirEntry dir_entry_buf[DIRENTS_PER_BLK];
    int dir_inode_index, dir_block_index, entries_read_num;
    char path_buf[MAX_PATH_TOKEN_SIZE*MAX_PATH_TOKEN_NUM];

    dir_inode_index = translate(path, &is_real_dir);
    //return error code
    if (dir_inode_index < 0) {
        return dir_inode_index;
    }
    //return error code
    if (!is_real_dir) {
        return -ENOTDIR;
    }
    //only have one data block
    dir_block_index = inodes[dir_inode_index].direct[0];
    entries_read_num = parser_dir_block(dir_block_index, dir_entry_buf);

    for (int i = 0; i < entries_read_num; i++) {
        struct stat sa;
        struct stat* sb = &sa;
        strcpy(path_buf, path);
        int path_len = strlen(path_buf);

        if (path_buf[path_len - 1] != '/')
            strcat(path_buf, "/");

        strcat(path_buf, dir_entry_buf[i].name);
        fs_getattr(path_buf, &sa);
        (*filler)(ptr, dir_entry_buf[i].name, &sa, 0);
    }
    return 0;
}

/**
 * open - open file directory.
 *
 * You can save information about the open directory in
 * fi->fh. If you allocate memory, free it in fs_releasedir.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @param fi fuse file system information
 * @return 0 if successful, or -error number
 */
static int fs_opendir(const char *path, struct fuse_file_info *fi)
{   
    uint8_t is_real_dir;
    int file_handler = translate(path, &is_real_dir);
    // return error code
    if (file_handler < 0)
        return file_handler;
    fi->fh = file_handler;
    return 0;
}

/**
 * Release resources when directory is closed.
 * If you allocate memory in fs_opendir, free it here.
 *
 * @param path the directory path
 * @param fi fuse file system information
 * @return 0 if successful, or -error number
 */
static int fs_releasedir(const char *path, struct fuse_file_info *fi)
{
    fi->fh = 0;
    return 0;
}

/**
 * mknod - create a new file with permissions (mode & 01777)
 * minor device numbers extracted from mode. Behavior undefined
 * when mode bits other than the low 9 bits are used.
 *
 * The access permissions of path are constrained by the
 * umask(2) of the parent process.
 *
 * Errors
 *   -ENOTDIR  - component of path not a directory
 *   -EEXIST   - file already exists
 *   -ENOSPC   - free inode not available
 *   -ENOSPC   - results in >32 entries in directory
 *
 * @param path the file path
 * @param mode the mode, indicating block or character-special file
 * @param dev the character or block I/O device specification
 * @return 0 if successful, or -error number
 */
static int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    char parent_path[MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE];
    char file_name_to_create[MAX_PATH_TOKEN_SIZE];
    DirEntry entries_parent[DIRENTS_PER_BLK];
    uint8_t is_real_dir, is_parent_dir;
    Inode *parent_dir_inode_ptr, *file_inode_ptr;
    int file_to_create_inode_idx, dir_parent_idx;
    int file_to_create_entry_idx;
    int file_to_create_blk_idx;
    uint8_t block_buf[BLOCK_SIZE];

    memset(parent_path, '\0', MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE);
    memset(file_name_to_create, '\0', MAX_PATH_TOKEN_SIZE);
    
    int test_inode_idx = translate(path, &is_real_dir);
    if (test_inode_idx > 0) {
        return -EEXIST;
    } 
    if (test_inode_idx == -ENOTDIR) {
        return -ENOTDIR;
    }
    //file inode
    file_to_create_inode_idx = get_free_inode();
    if (file_to_create_inode_idx == 0)
        return -ENOSPC;
    inodes[file_to_create_inode_idx].size = 0;
    inodes[file_to_create_inode_idx].mode = mode;
    memset(inodes[file_to_create_inode_idx].direct, 0, N_DIRECT);
    inodes[file_to_create_inode_idx].indir_1 = 0;
    inodes[file_to_create_inode_idx].indir_2 = 0;
    inodes[file_to_create_inode_idx].ctime = time(NULL);
    inodes[file_to_create_inode_idx].mtime = time(NULL);
    mark_inode(inodes + file_to_create_inode_idx);

    get_parent_dir(path, parent_path);
    strip_dir(path, file_name_to_create);
    
    dir_parent_idx = translate(parent_path, &is_parent_dir);
    parent_dir_inode_ptr = inodes + dir_parent_idx;
    read_block((parent_dir_inode_ptr -> direct)[0], (uint8_t*)entries_parent);
    //file entry
    file_to_create_entry_idx = find_free_dir(entries_parent);
    if (file_to_create_entry_idx == DIR_FULL) {
        return -ENOSPC;
    }
    entries_parent[file_to_create_entry_idx].valid = TRUE;
    strcpy(entries_parent[file_to_create_entry_idx].name, file_name_to_create);
    entries_parent[file_to_create_entry_idx].inode = file_to_create_inode_idx;
    entries_parent[file_to_create_entry_idx].isDir = FALSE;
    //write back entries
    write_block((parent_dir_inode_ptr -> direct)[0], (uint8_t*)entries_parent);
    flush_metadata();
    return 0;
}

/**
 *  mkdir - create a directory with the given mode. Behavior
 *  undefined when mode bits other than the low 9 bits are used.
 *
 * Errors
 *   -ENOTDIR  - component of path not a directory
 *   -EEXIST   - directory already exists
 *   -ENOSPC   - free inode not available
 *   -ENOSPC   - results in >32 entries in directory
 *
 * @param path path to file
 * @param mode the mode for the new directory
 * @return 0 if successful, or -error number
 */
static int fs_mkdir(const char *path, mode_t mode)
{   
    char parent_path[MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE];
    char file_name_to_create[MAX_PATH_TOKEN_SIZE];
    DirEntry entries_to_create[DIRENTS_PER_BLK], entries_parent[DIRENTS_PER_BLK];
    uint8_t is_real_dir, is_parent_dir;
    Inode* parent_dir_inode_ptr, *dir_inode_ptr;
    int dir_to_create_inode_idx, dir_parent_idx;
    int dir_to_create_entry_idx;
    int dir_to_create_blk_idx;
    uint8_t block_buf[BLOCK_SIZE];

    memset(parent_path, '\0', MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE);
    memset(file_name_to_create, '\0', MAX_PATH_TOKEN_SIZE);

    get_parent_dir(path, parent_path);
    strip_dir(path, file_name_to_create);
    dir_parent_idx = translate(parent_path, &is_parent_dir);
    parent_dir_inode_ptr = inodes + dir_parent_idx;
    read_block((parent_dir_inode_ptr -> direct)[0], (uint8_t*)entries_parent);
    if (find_in_dir(entries_parent, file_name_to_create) != NAME_NOT_FOUND) {
        return -EEXIST;
    }

    dir_to_create_entry_idx = find_free_dir(entries_parent);
    dir_to_create_inode_idx = get_free_inode();
    dir_to_create_blk_idx = get_free_blk();
    //cannot allocate empty inode and entry
    if (dir_to_create_entry_idx == DIR_FULL || dir_to_create_inode_idx == 0) {
        return -ENOSPC;
    }

    dir_inode_ptr = inodes + dir_to_create_inode_idx;
    (dir_inode_ptr -> direct)[0] = dir_to_create_blk_idx;
    dir_inode_ptr -> mode = mode | S_IFDIR;
    dir_inode_ptr -> ctime = time(NULL);
    dir_inode_ptr -> mtime = time(NULL);

    mark_inode(dir_inode_ptr);
    memset(block_buf, 0, BLOCK_SIZE);
    write_block(dir_to_create_blk_idx, block_buf);

    entries_parent[dir_to_create_entry_idx].valid = TRUE;
    strcpy(entries_parent[dir_to_create_entry_idx].name, file_name_to_create);
    entries_parent[dir_to_create_entry_idx].inode = dir_to_create_inode_idx;
    entries_parent[dir_to_create_entry_idx].isDir = TRUE;

    write_block((parent_dir_inode_ptr -> direct)[0], (uint8_t*)entries_parent);
    flush_metadata();
    
    return 0;
}

/**
 * truncate - truncate file to exactly 'len' bytes.
 *
 * Errors:
 *   ENOENT  - file does not exist
 *   ENOTDIR - component of path not a directory
 *   EINVAL  - length invalid (only supports 0)
 *   EISDIR	 - path is a directory (only files)
 *
 * @param path the file path
 * @param len the length
 * @return 0 if successful, or -error number
 */
static int fs_truncate(const char *path, off_t len)
{
    if (len != 0) {
    	return -EINVAL;		
    }
    uint8_t is_real_dir;
    int inode_idx = translate(path, &is_real_dir);
    Inode* inode_ptr;
    int blk_idx;
    int total_blocks;
    uint32_t indir2ptrs[PTRS_PER_BLK];

    if (inode_idx < 0) {
        return inode_idx;
    }
    if (is_real_dir) {
        return -EISDIR; 
    }
    inode_ptr = inodes + inode_idx;
    //release stored data blocks
    total_blocks = get_file_block_num(inode_ptr -> size);
    for (int i = 0; i < total_blocks; i++) {
    	blk_idx = get_blk(inode_ptr, i, FALSE);
        return_blk(blk_idx);
    }
    return_indir_ptrs_blocks(inode_ptr);
    
    inode_ptr -> size = 0;
    memset(inode_ptr -> direct, 0, sizeof(uint32_t) * N_DIRECT);
    inode_ptr -> indir_1 = 0;
    inode_ptr -> indir_2 = 0;
    mark_inode(inode_ptr);
    flush_metadata();
    return 0;
}

/**
 * unlink - delete a file.
 *
 * Errors
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *   -EISDIR   - cannot unlink a directory
 *
 * @param path path to file
 * @return 0 if successful, or -error number
 */
static int fs_unlink(const char *path)
{
    uint8_t is_real_dir, is_parent_dir;
    int dir_parent_idx, inode_idx, entries_blk_idx, entry_to_rm_idx;

    Inode* parent_inode_ptr = inodes + inode_idx;
    char parent_path[MAX_PATH_TOKEN_NUM * MAX_PATH_TOKEN_SIZE];
    char file_name_to_rm[MAX_PATH_TOKEN_SIZE];
    DirEntry entries_parent[DIRENTS_PER_BLK];

    memset(parent_path, '\0', MAX_PATH_TOKEN_NUM * MAX_PATH_TOKEN_SIZE);

    inode_idx = translate(path, &is_real_dir);
    if (inode_idx < 0) {
        return inode_idx;
    }
    if (is_real_dir) {
        return -EISDIR; 
    }
    //release stored data blocks
    fs_truncate(path, 0);
    //release inode
    return_inode(inode_idx);
    flush_metadata();

    get_parent_dir(path, parent_path);
    strip_dir(path, file_name_to_rm);
    
    //delete name in parent directory
    dir_parent_idx = translate(parent_path, &is_parent_dir);
    parent_inode_ptr = inodes + dir_parent_idx;
    entries_blk_idx = (parent_inode_ptr -> direct)[0];

    read_block(entries_blk_idx, (uint8_t*)entries_parent);
    entry_to_rm_idx = find_in_dir(entries_parent, file_name_to_rm);
    entries_parent[entry_to_rm_idx].valid = FALSE;
    //write back
    write_block(entries_blk_idx, (uint8_t*)entries_parent);
    return 0;
}

/**
 * rmdir - remove a directory.
 *
 * Errors
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *   -ENOTDIR  - path not a directory
 *   -ENOTEMPTY - directory not empty
 *
 * @param path the path of the directory
 * @return 0 if successful, or -error number
 */
static int fs_rmdir(const char *path)
{
    char parent_path[MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE];
    char file_name_to_rm[MAX_PATH_TOKEN_SIZE];
    memset(parent_path, '\0', MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE);
    memset(file_name_to_rm, '\0', MAX_PATH_TOKEN_SIZE);
    uint8_t is_real_dir, is_parent_dir;
    int dir_to_rm_inode_idx, dir_parent_idx;
    uint32_t entries_blk;
    int entry_to_rm_idx;
    Inode* inode_ptr_rm, *inode_ptr_parent;
    DirEntry entries_to_rm[DIRENTS_PER_BLK], entries_parent[DIRENTS_PER_BLK];

    dir_to_rm_inode_idx = translate(path, &is_real_dir);
    if (dir_to_rm_inode_idx < 0) {
        return dir_to_rm_inode_idx;
    } 
    if (!is_real_dir) {
        return -ENOTDIR;
    }

    inode_ptr_rm = inodes + dir_to_rm_inode_idx;
    entries_blk = (inode_ptr_rm -> direct)[0];
    read_block(entries_blk, (uint8_t*)entries_to_rm);
    //cannot delete non-empty directory
    if (!is_empty_dir(entries_to_rm)) {
        return -ENOTEMPTY;
    }
    return_blk(entries_blk);

    //delete inode
    return_inode(dir_to_rm_inode_idx);
    flush_metadata();
    
    get_parent_dir(path, parent_path);
    strip_dir(path, file_name_to_rm);

    //delete in parent directory's entries
    dir_parent_idx = translate(parent_path, &is_parent_dir);
    inode_ptr_parent = inodes + dir_parent_idx;
    entries_blk = (inode_ptr_parent -> direct)[0];
    read_block(entries_blk, (uint8_t*)entries_parent);

    entry_to_rm_idx = find_in_dir(entries_parent, file_name_to_rm);
    entries_parent[entry_to_rm_idx].valid = FALSE;
    //write back
    write_block(entries_blk, (uint8_t*)entries_parent);
    return 0;
}

/**
 * rename - rename a file or directory.
 *
 * Note that this is a simplified version of the UNIX rename
 * functionality - see 'man 2 rename' for full semantics. In
 * particular, the full version can move across directories, replace a
 * destination file, and replace an empty directory with a full one.
 *
 * Errors:
 *   -ENOENT   - source file or directory does not exist
 *   -ENOTDIR  - component of source or target path not a directory
 *   -EEXIST   - destination already exists
 *   -EINVAL   - source and destination not in the same directory
 *
 * @param src_path the source path
 * @param dst_path the destination path.
 * @return 0 if successful, or -error number
 */
static int fs_rename(const char *src_path, const char *dst_path)
{
    uint8_t is_parent_dir;
    int dir_parent_idx;
    Inode* inode_ptr_rm;
    DirEntry entries_parent[DIRENTS_PER_BLK];
    char parent_path_src[MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE];
    char parent_path_dst[MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE];
    char file_name_src[MAX_PATH_TOKEN_SIZE];
    char file_name_dst[MAX_PATH_TOKEN_SIZE];

    int entry_to_rename_idx, entries_blk_idx;

    memset(parent_path_src, '\0', MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE);
    memset(parent_path_dst, '\0', MAX_PATH_TOKEN_NUM*MAX_PATH_TOKEN_SIZE);
    memset(file_name_src, '\0', MAX_PATH_TOKEN_SIZE);
    memset(file_name_dst, '\0', MAX_PATH_TOKEN_SIZE);

    get_parent_dir(src_path, parent_path_src);
    get_parent_dir(dst_path, parent_path_dst);

    if (strcmp(parent_path_src, parent_path_dst) != 0) {
        return -EINVAL;
    }

    strip_dir(src_path, file_name_src);
    strip_dir(dst_path, file_name_dst);

    dir_parent_idx = translate(parent_path_src, &is_parent_dir);

    if (dir_parent_idx < 0) {
        return dir_parent_idx;
    } 
    if (!is_parent_dir) {
        return -ENOTDIR;
    }
    inode_ptr_rm = inodes + dir_parent_idx;
    //delete in parent directory's entries
    entries_blk_idx = (inode_ptr_rm -> direct)[0];
    read_block(entries_blk_idx, (uint8_t*)entries_parent);
    if(find_in_dir(entries_parent, file_name_dst) >= 0) {
        return EEXIST;
    }
    entry_to_rename_idx = find_in_dir(entries_parent, file_name_src);
    strcpy(entries_parent[entry_to_rename_idx].name, file_name_dst);
    //write back
    write_block(entries_blk_idx, (uint8_t*)entries_parent);
    return 0;
}

/**
 * chmod - change file permissions
 *
 * Errors:
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *
 * @param path the file or directory path
 * @param mode the mode_t mode value -- see man 'chmod'
 *   for description
 * @return 0 if successful, or -error number
 */
static int fs_chmod(const char *path, mode_t mode)
{
    int inode_idx;
    uint8_t is_real_dir;
    Inode* inode_ptr;
    inode_idx = translate(path, &is_real_dir);
    if (inode_idx < 0) {
        return inode_idx;
    }
    inode_ptr = inodes + inode_idx;
    inode_ptr -> mode = mode;
    mark_inode(inodes + inode_idx);
    flush_metadata();
    return 0;
}

/**
 * utime - change access and modification times.
 *
 * Errors:
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *
 * @param path the file or directory path.
 * @param ut utimbuf - see man 'utime' for description.
 * @return 0 if successful, or -error number
 */
int fs_utime(const char *path, struct utimbuf *ut)
{
    int inode_idx;
    uint8_t is_real_dir;
    Inode* inode_ptr;
    inode_idx = translate(path, &is_real_dir);
    if (inode_idx < 0) {
        return inode_idx;
    }
    inode_ptr = inodes + inode_idx;
    inode_ptr -> mtime = ut -> modtime;
    mark_inode(inodes + inode_idx);
    flush_metadata();
    return 0;
}

/**
 * read - read data from an open file.
 *
 * Should return exactly the number of bytes requested, except:
 *   - if offset >= file len, return 0
 *   - if offset+len > file len, return bytes from offset to EOF
 *   - on error, return <0
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *   -EISDIR  - file is a directory
 *   -EIO     - error reading block
 *
 * @param path the path to the file
 * @param buf the read buffer
 * @param len the number of bytes to read
 * @param offset to start reading at
 * @param fi fuse file info
 * @return number of bytes actually read if successful, or -error number
 */
static int fs_read(const char *path, char *buf, size_t len, off_t offset,
		    struct fuse_file_info *fi)
{
    Inode* inode_ptr = inodes + fi -> fh;
    int32_t file_size = inode_ptr -> size;
    int32_t size_to_return;
    uint8_t block_buf[BLOCK_SIZE];
    if (fi -> fh < 0)
        return fi -> fh;

    if (offset >= file_size)
        return 0;

    if (offset + len > file_size)
    	size_to_return = file_size - offset;
    else
    	size_to_return = len;

    int rest_length = len;
    int block_index_nth;
    int block_offset;
    int real_blk_idx;
    int buf_idx = 0;
    block_index_nth = offset / BLOCK_SIZE;
    block_offset = offset % BLOCK_SIZE;

    while (rest_length > 0) {
    	real_blk_idx = get_blk(inode_ptr, block_index_nth, FALSE);
        read_block(real_blk_idx, block_buf);
        for (int k = block_offset; k < BLOCK_SIZE; k++, buf_idx++) {
            buf[buf_idx] = block_buf[k];
        }
        rest_length -= (BLOCK_SIZE - block_offset);
        block_index_nth++;
        block_offset = 0;
    }
    return size_to_return;
}

/**
 *  write - write data to a file
 *
 * It should return exactly the number of bytes requested, except on
 * error.
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *   -EISDIR  - file is a directory
 *   -EINVAL  - if 'offset' is greater than current file length.
 *  			(POSIX semantics support the creation of files with
 *  			"holes" in them, but we don't)
 *
 * @param path the file path
 * @param buf the buffer to write
 * @param len the number of bytes to write
 * @param offset the offset to starting writing at
 * @param fi the Fuse file info for writing
 * @return number of bytes actually written if successful, or -error number
 *
 */
static int fs_write(const char *path, const char *buf, size_t len,
		     off_t offset, struct fuse_file_info *fi)
{
    Inode* inode_ptr = inodes + (fi -> fh);
    int32_t addition_size, addition_block_num;
    int32_t current_block_num, current_max_size;
    uint8_t block_buf[BLOCK_SIZE+1];

    current_block_num = get_file_block_num(inode_ptr -> size);
    current_max_size = current_block_num * BLOCK_SIZE;
    uint32_t old_size = inode_ptr -> size;
    if (len + offset >= inode_ptr -> size && len + offset < current_max_size) {
    	inode_ptr -> size = len + offset;
    }
    else if (len + offset >= current_max_size) {
    	//need new allocated space
    	addition_size = len + offset -  current_max_size;
        if (addition_size % BLOCK_SIZE == 0)
        	addition_block_num = addition_size / BLOCK_SIZE;
        else 
        	addition_block_num = addition_size / BLOCK_SIZE + 1;
        get_blk(inode_ptr, addition_block_num + current_block_num - 1, TRUE);
        inode_ptr -> size = len + offset;
    }

    int rest_length = len;
    int block_index_nth;
    int block_offset;
    int real_blk_idx;
    int buf_idx = 0;
    block_index_nth = offset / BLOCK_SIZE;
    block_offset = offset % BLOCK_SIZE;

    buf_idx = 0;
    while (rest_length > 0) {
    	real_blk_idx = get_blk(inode_ptr, block_index_nth, FALSE);
        for (int k = block_offset; k < BLOCK_SIZE; k++, buf_idx++) {
        	block_buf[k] = buf[buf_idx];
        }
        write_block(real_blk_idx, block_buf);
        rest_length -= (BLOCK_SIZE - block_offset);
        block_index_nth++;
        block_offset = 0;
    }
    mark_inode(inode_ptr);
    flush_metadata();
    return len;
}

/**
 * Open a filesystem file or directory path.
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *   -EISDIR  - file is a directory
 *
 * @param path the path
 * @param fuse file info data
 * @return 0 if successful, or -error number
 */
static int fs_open(const char *path, struct fuse_file_info *fi)
{
    uint8_t is_real_dir;
    fi -> fh = translate(path, &is_real_dir);
    if (fi -> fh < 0) {
        return fi -> fh;
    }
    if (is_real_dir) {
        return -EISDIR;
    }
    return 0;
}

/**
 * Release resources created by pending open call.
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *
 * @param path the file name
 * @param fi the fuse file info
 * @return 0 if successful, or -error number
 */
static int fs_release(const char *path, struct fuse_file_info *fi)
{
    fi -> fh = 0;
    return 0;
}

/**
 * statfs - get file system statistics.
 * See 'man 2 statfs' for description of 'struct statvfs'.
 *
 * Errors
 *   none -  Needs to work
 *
 * @param path the path to the file
 * @param st the statvfs struct
 * @return 0 for successful
 */
static int fs_statfs(const char *path, struct statvfs *st)
{
    st->f_bsize = FS_BLOCK_SIZE;
    st->f_blocks = sb.num_blocks - sb.inode_map_sz - sb.inode_region_sz - sb.block_map_sz - 1;  /* probably want to */
    st->f_bfree = count_free_blk();            /* change these */
    st->f_bavail = st->f_bfree;           /* values */
    st->f_namemax = FS_FILENAME_SIZE - 1;

    return 0;
}


/**
 * Operations vector. Please don't rename it, as the
 * skeleton code in misc.c assumes it is named 'fs_ops'.
 */
struct fuse_operations fs_ops = {
    .init = fs_init,
    .getattr = fs_getattr,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .releasedir = fs_releasedir,
    .mknod = fs_mknod,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .rename = fs_rename,
    .chmod = fs_chmod,
    .utime = fs_utime,
    .truncate = fs_truncate,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .release = fs_release,
    .statfs = fs_statfs,
};

