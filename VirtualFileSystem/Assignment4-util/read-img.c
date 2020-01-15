/*
 * file:        read-img.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

#include "fsx600.h"

/**
 * Read and print memory summary of cs/5600/7600 file system
 *
 * @param argv[0] name of image file system
 */
int main(int argc, char **argv)
{
	// open image file
    int i, j, fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("can't open"), exit(1);
    }

    // stat file to get size information
    struct stat _sb;
    if (fstat(fd, &_sb) < 0) {
        perror("fstat");
        exit(1);
    }
    int size = _sb.st_size;

    // read file into memory
    void *disk = malloc(size);
    if (read(fd, disk, size) != size) {
        perror("read");
        exit(1);
    }
    fd_set *blkmap = calloc(size/BITS_PER_BLK, 1);
    fd_set *imap = calloc(size/BITS_PER_BLK, 1);

    // report on superblock
    struct fs_super *sb = (void*)disk;
    printf("superblock: magic:  %08x\n"
           "            imap:   %d blocks\n" 
           "            bmap:   %d blocks\n"
           "            inodes: %d blocks\n" 
           "            blocks: %d\n"
           "            root inode: %d\n\n",
		   sb->magic, sb->inode_map_sz, sb->block_map_sz,
		   sb->inode_region_sz, sb->num_blocks, sb->root_inode);

    // report on inode map
    printf("allocated inodes: ");
    fd_set *inode_map = (void*)disk + FS_BLOCK_SIZE;
    char *comma = "";
    for (i = 0; i < sb->inode_map_sz * BITS_PER_BLK; i++) {
        if (FD_ISSET(i, inode_map)) {
            printf("%s %d", comma, i);
            comma = ",";
        }
    }
    printf("\n\n");

    // report on block map
    printf("allocated blocks: ");
    fd_set *block_map = (void*)inode_map + sb->inode_map_sz * FS_BLOCK_SIZE;
    for (comma = "", i = 0; i < sb->block_map_sz * BITS_PER_BLK; i++) {
        if (FD_ISSET(i, block_map)) {
            printf("%s %d", comma, i);
            comma = ",";
        }
    }
    printf("\n\n");

    // point to inodes
    struct fs_inode *inodes = (void*)block_map + sb->block_map_sz * FS_BLOCK_SIZE;

    int max_inodes = sb->inode_region_sz * INODES_PER_BLK;
    struct entry { int dir; int inum;} inode_list[max_inodes + 100];
    int head = 0, tail = 0;

    inode_list[head++] = (struct entry){.dir=1, .inum=1};
    FD_SET(1, imap);
    while (head != tail) {
        struct entry e = inode_list[tail++];
        struct fs_inode *in = inodes + e.inum;
        if (!e.dir) {
        	// report on inode info
            printf("file: inode %d\n"
                   "      uid/gid %d/%d\n"
                   "      mode %08o\n"
                   "      size  %d\n",
                   e.inum, in->uid, in->gid, in->mode, in->size);
            printf("blocks: ");

            // report on direct blocks
            for (i = 0; i < N_DIRECT; i++) {
                if (in->direct[i] != 0) {
                    printf("%d ", in->direct[i]);
                    FD_SET(in->direct[i], blkmap);
                    if (!FD_ISSET(in->direct[i], block_map))
                        printf("\n***ERROR*** block %d marked free\n", in->direct[i]);
                }
            }

            // report on single indirect blocks
            if (in->indir_1 != 0) {
                int *buf = disk + in->indir_1 * FS_BLOCK_SIZE;
                for (i = 0; i < PTRS_PER_BLK; i++) {
                    if (buf[i] != 0) {
                        printf("%d ", buf[i]);
                        FD_SET(buf[i], blkmap);
                        if (!FD_ISSET(buf[i], block_map)) {
                            printf("\n***ERROR*** block %d marked free\n", buf[i]);
                        }
                    }
                }
            }

            // report on double indirect blocks
            if (in->indir_2 != 0) {
                int *buf2 = disk + in->indir_2 * FS_BLOCK_SIZE;
                // scan indirect block
                for (i = 0; i < PTRS_PER_BLK; i++) {
                    if (buf2[i] != 0) {
                    	// scan double-indirect block
                        int *buf = disk + buf2[i] * FS_BLOCK_SIZE;
                        for (j = 0; j < PTRS_PER_BLK; j++) {
                            if (buf[j] != 0) {
                                printf("%d ", buf[j]);
                                FD_SET(buf[j], blkmap);
                                if (!FD_ISSET(buf[j], block_map)) {
                                    printf("\n***ERROR*** block %d marked free\n", buf[j]);
                                }
                            }
                        }
                    }
                }
            }
            printf("\n\n");
        }
        else {
        	// report on directory
            if (!S_ISDIR(in->mode)) {
                printf("***ERROR*** inode %d not a directory\n", e.inum);
                continue;
            }
            printf("directory: inode %d (block %d)\n", e.inum, in->direct[0]);
            struct fs_dirent *de = disk + in->direct[0] * FS_BLOCK_SIZE;
            if (!FD_ISSET(in->direct[0], block_map)) {
                printf("\n***ERROR*** block %d marked free\n", in->direct[0]);
            }
            FD_SET(in->direct[0], blkmap);
            
            // scan directory block
            for (i = 0; i < DIRENTS_PER_BLK; i++) {
                if (de[i].valid) {
                	// report on valid directory entry
                    printf("  %s %d %s\n", de[i].isDir ? "D" : "F", de[i].inode,
                           de[i].name);
                    int j = de[i].inode;
                    if (j < 0 || j >= sb->inode_region_sz * 16) {
                        printf("***ERROR*** invalid inode %d\n", j);
                        continue;
                    }
                    if (FD_ISSET(j, imap)) {
                        printf("***ERROR*** loop found (inode %d)\n", e.inum);
                        goto fail;
                    }
                    FD_SET(j, imap);
                    if (!FD_ISSET(j, inode_map)) {
                        printf("***ERROR*** inode %d is marked free\n", j);
                    }
                    inode_list[head++] = (struct entry) {.dir = de[i].isDir, j};
                }
            }
            printf("\n");
        }
    }

    // report on unreachable inodes
    printf("unreachable inodes: ");
    for (i = 1; i < sb->inode_region_sz * 16; i++) {
        if (!FD_ISSET(i, imap) && FD_ISSET(i, inode_map)) {
            printf("%d ", i);
        }
    }
    printf("\n");

    // report on unreachable blocks
    printf("unreachable blocks: ");
    for (i = 1 + sb->inode_map_sz + sb->block_map_sz + sb->inode_region_sz;
         i < sb->num_blocks; i++) {
        if (FD_ISSET(i, blkmap) && !FD_ISSET(i, block_map)) {
            printf("%d ", i);
        }
    }
    printf("\n");

fail:
    return 0;
}
