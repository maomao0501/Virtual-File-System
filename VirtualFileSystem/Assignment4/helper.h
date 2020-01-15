#define FALSE 0
#define TRUE  1
#define MAX_PATH_TOKEN_NUM  100
#define MAX_PATH_TOKEN_SIZE 40

#define IMAP_DIRTY 1
#define BMAP_DIRTY 2
#define INOD_DIRTY 3

#define INODE_MAP 1
#define BLOCK_MAP 2

#define NAME_NOT_FOUND -1
#define DIR_FULL -1

static int count_free_blk(void);
static int get_blk(struct fs_inode *in, int n, int alloc);
static int get_file_block_num(int32_t size);
static int is_empty_dir(struct fs_dirent *de);
static int find_free_dir(struct fs_dirent *de);
static int find_in_dir(struct fs_dirent *de, char *name);
static void return_inode(int inum);
static int get_free_inode(void);
static void return_blk(int blkno);
static int get_free_blk(void);
static void return_indir_ptrs_blocks(Inode* inodePtr);
static void get_parent_dir(const char* path, char* parentPath);
static void flush_metadata(void);
static void mark_inode(struct fs_inode *in);
static int translate_1(const char *path, char *leaf);
static int translate(const char *path, uint8_t* isRealDir);
static int parse(const char *path, char **names, int nnames);
static int lookup(int inum, char *name, uint8_t* isRealDir);
static int parser_dir_block(int dirBlkIndex, DirEntry* dirEntries);
static void write_block(uint32_t blk_index, const uint8_t* data_buf);
static void read_block(uint32_t blk_index, uint8_t* data_buf);
static void write_block(uint32_t blk_index, const uint8_t* data_buf);
static void read_block(uint32_t blk_index, uint8_t* data_buf);
static void strip_dir(const char* path, char *nodirFilename);

/**
 * Reading blocks from block device.
 * @param blk_index
 * @param data_buf
 *
 */
static void read_block(uint32_t blk_index, uint8_t* data_buf) {
    if (disk->ops->read(disk, blk_index, 1, (void*)data_buf) < 0) {
        printf("block reading error %u\n", blk_index);
        exit(1);
    }
}

/**
 * Writing blocks to block device.
 * @param blk_index
 * @param data_buf
 *
 */
static void write_block(uint32_t blk_index, const uint8_t* data_buf) {
    if (disk->ops->write(disk, blk_index, 1, (void*)data_buf) < 0) {
        printf("block writing error %u\n", blk_index);
        exit(1);
    }
}


/**
 * Parse a data block storing a directory entry into an array
 * @param dir_blk_index
 * @param dir_entries
 *
 */
static int parser_dir_block(int dir_blk_index, DirEntry* dir_entries) {
    uint8_t blk_buf[FS_BLOCK_SIZE];
    int wr_idx, i;
    DirEntry* tmp_entry;
    read_block(dir_blk_index, blk_buf);
    for (i = 0, wr_idx=0; i < DIRENTS_PER_BLK; i++) {
    	tmp_entry = (DirEntry*)(blk_buf + i * sizeof(DirEntry));
        if (tmp_entry -> valid)
        	dir_entries[wr_idx++] = *tmp_entry;
    }
    return wr_idx;
}

/* Suggested functions to implement -- you are free to ignore these
 * and implement your own instead
 */

/**
 * Look up a single directory entry in a directory.
 *
 * Errors
 *   -EIO     - error reading block
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - intermediate component of path not a directory
 *
 */
static int lookup(int inum, char *name, uint8_t* is_real_dir)
{
    int dir_block_index = inodes[inum].direct[0];
    uint32_t name_length = strlen(name);
    uint8_t isdir = name[name_length - 1] == '/';
    char pure_name[MAX_PATH_TOKEN_SIZE];
    int entries_read_num = 0;
    DirEntry dir_entry_buf[DIRENTS_PER_BLK];
    int inode_found;
    memset(pure_name, '\0', MAX_PATH_TOKEN_SIZE);
    if (isdir) 
        strncpy(pure_name, name, name_length-1);
    else 
        strncpy(pure_name, name, name_length);
    entries_read_num = parser_dir_block(dir_block_index, dir_entry_buf);

    //read almost DIRENTS_PER_BLK's entry
    assert(entries_read_num <= DIRENTS_PER_BLK);
    for (int j = 0; j < entries_read_num; j++) {
        if (strcmp(dir_entry_buf[j].name, pure_name) == 0) {
            //file name is directory but its not a directory
            if (isdir && !dir_entry_buf[j].isDir) {
                return -ENOTDIR;
            }
            inode_found = dir_entry_buf[j].inode;
            *is_real_dir = dir_entry_buf[j].isDir ? TRUE: FALSE;
            return inode_found;
        }
    }
    return -ENOENT;
}

/**
 * Parse path name into tokens at most nnames tokens after
 * normalizing paths by removing '.' and '..' elements.
 *
 * If names is NULL,path is not altered and function  returns
 * the path count. Otherwise, path is altered by strtok() and
 * function returns names in the names array, which point to
 * elements of path string.
 *
 * @param path the directory path
 * @param names the argument token array or NULL
 * @param nnames the maximum number of names, 0 = unlimited
 * @return the number of path name tokens
 */
static int parse(const char *path, char **names, int nnames)
{ 
    assert(nnames == 0);
    char tmp_file_name[MAX_PATH_TOKEN_SIZE];
    int idx;
    //first filename
    int file_name_wr_idx = 0;
    const char *file_name_start_ptr;
    idx = 0;
    file_name_start_ptr = path;

    while (path[idx] != '\0') {
        if ((path[idx] == '/') || (path[idx + 1] == '\0' && path[idx] != '/')) { 
            //copy directory path
            memset(tmp_file_name, '\0', MAX_PATH_TOKEN_SIZE);
            strncpy(tmp_file_name, file_name_start_ptr, path + idx + 1 - file_name_start_ptr);
            file_name_start_ptr = path + idx + 1;
            strcpy(names[file_name_wr_idx], tmp_file_name);
            file_name_wr_idx++;
        }
        idx++;
    } 
    return file_name_wr_idx;
}


/* Return inode number for specified file or
 * directory.
 * 
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @return inode of path node or error
 */
static int translate(const char *path, uint8_t* is_real_dir)
{
	char *names[MAX_PATH_TOKEN_NUM];
	char name_storage[MAX_PATH_TOKEN_NUM][MAX_PATH_TOKEN_SIZE];
	int token_nums = 0;
	int tmp_inode_index;
	Inode tmp_inode;
	for (int i = 0; i < MAX_PATH_TOKEN_NUM; i++) {
		names[i] = name_storage[i];
	}
	//split the path into multiple tokens
	token_nums = parse(path, names, 0);
	//root inode
	tmp_inode_index = sbPtr -> root_inode;
	tmp_inode = inodes[tmp_inode_index];
	*is_real_dir=TRUE;
	//from root path
	for (int i = 1; i < token_nums; i++) {
		tmp_inode_index = lookup(tmp_inode_index, names[i], is_real_dir);
		//return error code
		if (tmp_inode_index < 0)
			return tmp_inode_index;
		else
			tmp_inode = inodes[tmp_inode_index];
	}
	return tmp_inode_index;
}

/**
 *  Return inode number for path to specified file
 *  or directory, and a leaf name that may not yet
 *  exist.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @param leaf pointer to space for FS_FILENAME_SIZE leaf name
 * @return inode of path node or error
 */
static int translate_1(const char *path, char *leaf)
{

    return -EOPNOTSUPP;
}

/**
 * Mark a inode as dirty.
 *
 * @param in pointer to an inode
 */
static void mark_inode(struct fs_inode *in)
{
    int inum = in - inodes;
    int blk = inum / INODES_PER_BLK;
    dirty[inode_base + blk] = (void*)inodes + blk * FS_BLOCK_SIZE;
}

/**
 * Flush dirty metadata blocks to disk.
 */
static void flush_metadata(void)
{
    int i;
    dirty[1] = inode_map;
    dirty[2] = block_map;
    for (i = 0; i < dirty_len; i++) {
        if (dirty[i]) {
            disk->ops->write(disk, i, 1, dirty[i]);
            dirty[i] = NULL; 
        }
    }
}

/**
 * Get the parent directory.
 *
 * @param path
 * @param parent_path
 */
static void get_parent_dir(const char* path, char* parent_path) {
    //parentpath need to set as 0
    int length = strlen(path);
    // root
    if (length == 1) {
        return ;
    }
    int i = path[length - 1] != '/' ? length - 1: length - 2;
    while (path[i] != '/') 
        i--;
    strncpy(parent_path, path, i + 1);
} 

/**
 * Strip the directory.
 *
 * @param path
 * @param nodir_filename
 */
static void strip_dir(const char* path, char *nodir_filename) {
    int length = strlen(path);
    // root
    if (length == 1) {
    	nodir_filename[0] = '/';
    	nodir_filename[1] = '\0';
        return ;
    }

    int i = path[length - 1] != '/' ? length - 1: length - 2;
    while (path[i] != '/')  {
        i--;
    }
    i++;
    int k = 0;
    while (path[i] != '/' && path[i] != '\0') {
    	nodir_filename[k++] = path[i++];
    }
} 

/**
 * Return the indir pointers blocks.
 * @param inode_ptr
 *
 */
static void return_indir_ptrs_blocks(Inode* inode_ptr) {
    uint32_t indir2;
    indir2 = inode_ptr -> indir_2;
    uint32_t indir2s[PTRS_PER_BLK];
    if (inode_ptr -> indir_1 != 0)
        return_blk(inode_ptr -> indir_1);
    if (indir2 != 0) {
        read_block(indir2, (uint8_t*)indir2s);
        for (int i = 0; i < PTRS_PER_BLK; i++) {
            if (indir2s[i]!=0) {
                return_blk(indir2s[i]);
            }
        } 
        return_blk(inode_ptr -> indir_2);
    }
}


/**
 * Returns a free block number or 0 if none available.
 *
 * @return free block number or 0 if none available
 */
static int get_free_blk(void)
{
    //from block
    int start_idx = sb.inode_map_sz + sb.inode_region_sz + sb.block_map_sz + 1;
    for (int i = start_idx; i < sb.num_blocks; i++) {
        if (!FD_ISSET(i, block_map)) {
            FD_SET(i, block_map);
            return i;
        }
    }
    return 0;
}

/**
 * Count the free block.
 *
 */
static int count_free_blk(void) {
    int cnt = 0;
    int start_idx = sb.inode_map_sz + sb.inode_region_sz + sb.block_map_sz + 1;
    for (int i = start_idx; i < sb.num_blocks; i++) {
        if (!FD_ISSET(i, block_map)) {
            cnt++;
        }
    }
    return cnt;
}

/**
 * Return a block to the free list
 *
 * @param  blkno the block number
 */
static void return_blk(int blkno)
{
    FD_CLR(blkno, block_map);
}

/**
 * Returns a free inode number
 *
 * @return a free inode number or 0 if none available
 */
static int get_free_inode(void)
{
    for (int i = sb.root_inode; i < sb.inode_region_sz * INODES_PER_BLK; i++) {
        if (!FD_ISSET(i, inode_map)) {
            FD_SET(i, inode_map);
            return i;
        }
    }
    return 0;
}

/**
 * Return a inode to the free list.
 *
 * @param  inum the inode number
 */
static void return_inode(int inum)
{
    FD_CLR(inum, inode_map);
}

/**
 * Find inode for existing directory entry.
 *
 * @param fs_dirent ptr to first dirent in directory
 * @param name the name of the directory entry
 * @return the entry inode, or 0 if not found.
 */
static int find_in_dir(struct fs_dirent *de, char *name)
{
    int i;
    DirEntry* tmp_entry_ptr;
    for (i = 0; i < DIRENTS_PER_BLK; i++) {
    	tmp_entry_ptr = de + i;
        if (tmp_entry_ptr -> valid && strcmp(tmp_entry_ptr -> name, name) == 0)
            return i;
    }
    return NAME_NOT_FOUND;
}

/**
 * Find free directory entry.
 *
 * @return index of directory free entry or -ENOSPC
 *   if no space for new entry in directory
 */
static int find_free_dir(struct fs_dirent *de)
{
    int i;
    DirEntry* tmp_entry_ptr;
    for (i = 0; i < DIRENTS_PER_BLK; i++) {
    	tmp_entry_ptr = de + i;
        if (!(tmp_entry_ptr -> valid))
            return i;
    }
    return DIR_FULL;
}

/**
 * Determines whether directory is empty.
 *
 * @param de ptr to first entry in directory
 * @return 1 if empty 0 if has entries
 */
static int is_empty_dir(struct fs_dirent *de)
{
    int i;
    DirEntry* tmp_entry_ptr;
    for (i = 0; i < DIRENTS_PER_BLK; i++) {
    	tmp_entry_ptr = de + i;
        if (tmp_entry_ptr -> valid)
            return FALSE;
    }
    return TRUE;
}

/**
 * Get the file block numbers.
 *
 * @param size
 */
static int get_file_block_num(int32_t size) {
    int8_t no_complete = size % BLOCK_SIZE != 0;
    int current_block_num = (size / BLOCK_SIZE) + no_complete;
    return current_block_num;
}

/**
 * Returns the n-th block of the file, or allocates
 * it if it does not exist and alloc == 1.
 *
 * @param in the file inode
 * @param n the 0-based block index in file
 * @param alloc 1=allocate block if does not exist 0 = fail
 *   if does not exist
 * @return block number of the n-th block or 0 if available
 */
static int get_blk(struct fs_inode *in, int n, int alloc)
{
    int current_block_num = get_file_block_num(in -> size);
    //assert(n >= 0 && n <= currentBlockNum);
    if (!alloc || (alloc && n < current_block_num)) {
        if (n < N_DIRECT) {
            return (in -> direct)[n];
        }
        else if (n >= N_DIRECT && n < N_DIRECT + PTRS_PER_BLK) {
            uint32_t ptrs[PTRS_PER_BLK];
            read_block(in -> indir_1, (uint8_t*)ptrs);
            return ptrs[n - N_DIRECT];
        }
        else {
            uint32_t ptrs_ptrs[PTRS_PER_BLK];
            uint32_t ptrs[PTRS_PER_BLK];
            int n_offset2 = n - N_DIRECT - PTRS_PER_BLK;
            int ptrs_ptrs_offset = n_offset2 / PTRS_PER_BLK;
            int ptrs_offset = n_offset2 % PTRS_PER_BLK;
            read_block(in -> indir_2, (uint8_t*)ptrs_ptrs);
            read_block(ptrs_ptrs[ptrs_ptrs_offset], (uint8_t*)ptrs);
            return ptrs[ptrs_offset];
        }
    }
    //allocate new blocks
    else {
        int new_allocate_num = n - current_block_num + 1;
        uint32_t ptrs_ptrs[PTRS_PER_BLK];
        uint32_t ptrs[PTRS_PER_BLK];
        int ptrs_ptrs_blk_idx;
        int ptrs_blk_idx;
        int ptrs_offset = 0;
        int ptrs_ptrs_offset = 0;
        int n_offset2 = 0;

        for (int i = 0; i < new_allocate_num; i++) {
            int cur_nth_block = current_block_num + i;
            int new_block_index = get_free_blk();
            if (cur_nth_block < N_DIRECT) {
                (in -> direct)[cur_nth_block] = new_block_index;
                mark_inode(in);
                //flush meta
            }
            else if (cur_nth_block >= N_DIRECT && cur_nth_block < N_DIRECT + PTRS_PER_BLK) {
                if (cur_nth_block == N_DIRECT) {
                    ptrs_blk_idx = get_free_blk();
                    in -> indir_1 = ptrs_blk_idx;
                    mark_inode(in);
                    flush_metadata();
                }
                else {
                    ptrs_blk_idx = in -> indir_1;
                    read_block(ptrs_blk_idx, (uint8_t*)ptrs);
                }
                ptrs[cur_nth_block - N_DIRECT] = new_block_index;
                if (cur_nth_block == N_DIRECT + PTRS_PER_BLK - 1 || cur_nth_block == n) {
                    write_block(ptrs_blk_idx, (uint8_t*)ptrs);
                }
            }
            else {     
                n_offset2 = cur_nth_block - N_DIRECT - PTRS_PER_BLK;
                ptrs_ptrs_offset = n_offset2 / PTRS_PER_BLK;
                ptrs_offset = n_offset2 % PTRS_PER_BLK;
                if (cur_nth_block == N_DIRECT + PTRS_PER_BLK) {
                    ptrs_ptrs_blk_idx = get_free_blk();
                    in -> indir_2 = ptrs_ptrs_blk_idx;
                    mark_inode(in);
                }

                if (ptrs_offset == 0) {
                    ptrs_blk_idx = get_free_blk();
                    ptrs_ptrs[ptrs_ptrs_offset] = ptrs_blk_idx;
                }

                ptrs[ptrs_offset] = new_block_index;

                if (ptrs_offset == PTRS_PER_BLK - 1 || cur_nth_block == n) {
                    write_block(ptrs_ptrs[ptrs_ptrs_offset], (uint8_t*)ptrs);
                }

                assert (cur_nth_block <= n);
                if (cur_nth_block == n) {
                    write_block(in -> indir_2, (uint8_t*)ptrs_ptrs);
                }
            }
        }
        flush_metadata();
    }
    return 0;
}
