#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include "wfs.h"

void *mapped_region;
struct wfs_sb *superblock;

int my_getattr(const char *path, struct stat *stbuf) {
    printf("Getting attributes for: %s\n", path);
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        // Root directory
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    // Locate the inode for the given path
    struct wfs_inode *inode = NULL;
    struct wfs_inode *inode_table = (struct wfs_inode *)((char *)mapped_region + superblock->i_blocks_ptr);
    for (int i = 0; i < superblock->num_inodes; i++) {
        if (inode_table[i].nlinks > 0) {
            struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)mapped_region + inode_table[i].blocks[0]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (strcmp(dentry_table[j].name, path + 1) == 0) {  // Remove leading '/'
                    inode = &inode_table[dentry_table[j].num];
                    break;
                }
            }
        }
    }

    if (!inode) {
        return -ENOENT;  // File or directory not found
    }

    // Populate the stat structure
    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    return 0;
}


int my_mknod() {
    printf("hello2\n");
    return 0;
}

int my_mkdir(const char *path, mode_t mode) {
    printf("Creating directory: %s\n", path);

    // Locate the parent directory (assume root directory "/" for simplicity)
    struct wfs_inode *parent_inode = (struct wfs_inode *)((char *)mapped_region + superblock->i_blocks_ptr);
    if (!parent_inode) {
        return -ENOENT;
    }

    // Find a free inode
    struct wfs_inode *inode_table = (struct wfs_inode *)((char *)mapped_region + superblock->i_blocks_ptr);
    struct wfs_inode *new_inode = NULL;
    for (int i = 0; i < superblock->num_inodes; i++) {
        if (inode_table[i].nlinks == 0) {
            new_inode = &inode_table[i];
            break;
        }
    }

    if (!new_inode) {
        return -ENOSPC;  // No free inodes
    }

    // Initialize the new inode
    memset(new_inode, 0, sizeof(struct wfs_inode));
    new_inode->mode = S_IFDIR | mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->nlinks = 2;  // "." and parent
    new_inode->size = 0;
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);

    // Find a free directory entry in the parent directory
    struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)mapped_region + parent_inode->blocks[0]);
    for (int i = 0; i < BLOCK_SIZE / sizeof(struct wfs_dentry); i++) {
        if (dentry_table[i].num == 0) {
            strncpy(dentry_table[i].name, path + 1, MAX_NAME);  // Remove leading '/'
            dentry_table[i].num = new_inode - inode_table;
            parent_inode->nlinks++;
            return 0;
        }
    }

    return -ENOSPC;  // No free directory entries
}


int my_unlink() {
    printf("hello4\n");
    return 0;
}

int my_rmdir() {
    printf("hello5\n");
    return 0;
}

int my_read() {
    printf("hello6\n");
    return 0;
}

int my_write() {
    printf("hello7\n");
    return 0;
}

int my_readdir() {
    printf("hello8\n");
    return 0;
}


static struct fuse_operations ops = {
    .getattr = my_getattr,
    .mknod   = my_mknod,
    .mkdir   = my_mkdir,
    .unlink  = my_unlink,
    .rmdir   = my_rmdir,
    .read    = my_read,
    .write   = my_write,
    .readdir = my_readdir,
};



int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk1> <disk2> ... <mount_point> [FUSE options]\n", argv[0]);
        return -1;
    }

    printf("ARGV ORIGINALLY\n");
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d]: %s\n", i, argv[i]);
    }

    // Identify disk image arguments
    int num_disks = 0;
    while (num_disks + 1 < argc && argv[num_disks + 1][0] != '-') {
        num_disks++;
    }

    if (num_disks < 2) {
        fprintf(stderr, "Error: At least two disk images are required.\n");
        return -1;
    }

    // Debug: Print disk images
    printf("Disk images:\n");
    for (int i = 0; i < num_disks; i++) {
        printf("  Disk %d: %s\n", i + 1, argv[i + 1]);
    }

    // Create a new array for FUSE arguments
    int fuse_argc = argc - num_disks - 1; // Exclude disk images and argv[0]
    char **fuse_argv = malloc(fuse_argc * sizeof(char *));
    if (!fuse_argv) {
        perror("Error allocating memory for FUSE arguments");
        return -1;
    }

    // Copy FUSE-related arguments to fuse_argv (skip argv[0] and disk image paths)
    for (int i = num_disks + 1; i < argc; i++) {
        fuse_argv[i - num_disks - 1] = argv[i];
    }

    // Debug: Print updated argc and argv for FUSE
    printf("FUSE argc: %d\n", fuse_argc);
    printf("FUSE argv:\n");
    for (int i = 0; i < fuse_argc; i++) {
        printf("  argv[%d]: %s\n", i, fuse_argv[i]);
    }

    // Start FUSE
    int result = fuse_main(fuse_argc, fuse_argv, &ops, NULL);

    // Free allocated memory
    free(fuse_argv);

    return result;
}
