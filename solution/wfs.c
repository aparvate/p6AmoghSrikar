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

void **mapped_regions;
int *fds;

// struct wfs_sb *superblock;


struct wfs_inode *find_inode_from_num (int num) {

    printf("find inode from num starting\n");

    struct wfs_sb *superblock = (struct wfs_sb *) mapped_regions[0];
    printf("checkpoint 1\n");
    int bits = 32;

    // Access the inode bitmap directly using the offset
    uint32_t *inode_bitmap = (uint32_t *)((char *) mapped_regions[0] + superblock->i_bitmap_ptr);

    printf("checkpoint 2\n");

    int bit_idx = num % bits;
    int array_idx = num / bits;

    printf("checkpoint 3\n");

    if (!(inode_bitmap[array_idx] & (0x1 << bit_idx))) {
        printf("checkpoint 4\n");
        return NULL;
    }

    printf("checkpoint 5\n");

    char *inode_table = ((char *) mapped_regions[0]) + superblock->i_blocks_ptr;
    printf("checkpoint 6\n");
    printf("find inode from num finished\n");
    return (struct wfs_inode *)((num * BLOCK_SIZE) + inode_table);
}



struct wfs_inode *locate_inode (const char* path) {

    printf("locate inode starting\n");

    printf("this is the path: %s\n", path);

    // Start with the root inode
    struct wfs_inode *curr_inode = find_inode_from_num(0);

    if (strcmp(path, "/") == 0) {
        return curr_inode;
    }

    char *temp_path = strdup(path);
    if (!temp_path) {
        return NULL;
    }

    // Tokenize the path
    char *token = strtok(temp_path, "/");
    if (!token) {
        free(temp_path);
        return NULL;
    }
    
    // check if root node is inode
    if (!curr_inode) {
        free(temp_path);
        return NULL;
    }

    // check if curr inode is directory
    if (!(curr_inode->mode & S_IFDIR)) {
        free(temp_path);
        return NULL;
    }

    // Traverse tokens
    while (token) {
        bool found = false;

        // Iterate through directory blocks of the current inode
        for (int i = 0; i < D_BLOCK; i++) {

            // doesn't have blocks to check
            if (curr_inode->blocks[i] == 0) {
                continue;
            } 

            struct wfs_sb *superblock = (struct wfs_sb *) mapped_regions[0];
            // first get the mapped region offset, then the block we are on, then the offset within the block
            char* location = (char *) mapped_regions[0] + superblock->d_blocks_ptr + (curr_inode->blocks[i] * BLOCK_SIZE);

            // Read directory entries from the block
            struct wfs_dentry *dentry= (struct wfs_dentry *)(location);

            for (size_t j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {

                if (strcmp(dentry[j].name, token) == 0) {
                    
                    // this is now the "root"
                    // do this process again from the while loop with this as root now

                    printf("in locate_inode, just called find_inode_from_num\n");
                    curr_inode = find_inode_from_num(dentry[j].num);
                    printf("in locate_inode, just finished find_inode_from_num\n");

                    found = true;
                    break;
                }
            }
            if (found) {
                break; // Exit directory block loop
            }
        }

        if (!found) {
            // Path component not found
            free(temp_path);
            return NULL;
        }
        else {
            // Get the next token
            token = strtok(NULL, "/");
        }
    }

    printf("locate inode finished successfully\n");

    free(temp_path);
    return curr_inode;
}







int wfs_getattr(const char *path, struct stat *stbuf) {

    printf("Get attribute starting\n");
    memset(stbuf, 0, sizeof(struct stat));
    printf("Memory set\n");

    if (strcmp(path, "/") == 0) {
        // Root directory
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    // Locate the file or directory in the inode table

    printf("in my_getattr, just called locate_inode\n");
    struct wfs_inode *inode = locate_inode(path);
    printf("in my_getattr, just finished locate_inode\n");
    if (!inode) {
        printf("No INODE!\n");
        return -ENOENT;
    }
    printf("Found INODE\n");

    printf("inode num is %i\n", inode->num);

    // Populate stat structure
    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;
    printf("Stat populated\n");

    return 0;
}

int wfs_mknod() {
    printf("hello2\n");
    return 0;
}

int wfs_mkdir(const char *path, mode_t mode) {
    
    printf("Starting mkdir\n");
    struct wfs_sb *superblock = (struct wfs_sb *) mapped_regions[0];
    struct wfs_inode *inode_table = (struct wfs_inode *)((char *)mapped_regions[0] + superblock->i_blocks_ptr);
    size_t num_inodes = superblock->num_inodes;
    printf("Superblock set, table set, num_inodes set\n");

    // Find a free inode
    struct wfs_inode *new_inode = NULL;
    for (size_t i = 0; i < num_inodes; i++) {
      printf("inode: %zd \n", i);
      if (inode_table[i].nlinks == 0) {
        new_inode = &inode_table[i];
        break;
      }
    }

    if (!new_inode) {
      printf("No INODE\n");
      return -ENOSPC;
    }

    // Initialize the new inode
    memset(new_inode, 0, sizeof(struct wfs_inode));
    new_inode->mode = S_IFDIR | mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->nlinks = 2;
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);

    // Find a free directory entry
    struct wfs_inode *parent_inode = locate_inode("/"); // Assuming root for simplicity
    if (!parent_inode) {
      printf("Root INODE not found");
      return -ENOENT;
    }

    struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)mapped_regions[0] + parent_inode->blocks[0]);
    for (size_t i = 0; i < BLOCK_SIZE / sizeof(struct wfs_dentry); i++) {
      printf("dentry: %zd \n", i);
      if (dentry_table[i].num == 0) {
        printf("Creating new directory");
        strncpy(dentry_table[i].name, path, MAX_NAME);
        dentry_table[i].num = new_inode - inode_table; // Index of the new inode
        parent_inode->nlinks++;
        return 0;
      }
    }

    return -ENOSPC;

    // printf("hai3\n");
    // return 0;
}

int wfs_unlink() {
    printf("hello4\n");
    return 0;
}

int wfs_rmdir() {
    printf("hello5\n");
    return 0;
}

int wfs_read() {
    printf("hello6\n");
    return 0;
}

int wfs_write() {
    printf("hello7\n");
    return 0;
}

int wfs_readdir() {
    printf("hello8\n");
    return 0;
}


static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod   = wfs_mknod,
    .mkdir   = wfs_mkdir,
    .unlink  = wfs_unlink,
    .rmdir   = wfs_rmdir,
    .read    = wfs_read,
    .write   = wfs_write,
    .readdir = wfs_readdir,
};



int main(int argc, char *argv[]) {

    if (argc < 3) {
        return -1;
    }

  // Identify disk image arguments
  int num_disks = 0;
  while (num_disks + 1 < argc && argv[num_disks + 1][0] != '-') {
    num_disks++;
  }

  mapped_regions = malloc(num_disks * sizeof(void *));
  fds = malloc(num_disks * sizeof(int));

    for(int i=0; i<num_disks; i++) {
        fds[i] = open(argv[i + 1], O_RDWR);
        printf("fds[i] == %i\n", fds[i]);


        struct stat temp;
        if (fstat(fds[i], &temp) != 0) {
            printf(" this si whats wrong fds[%i] == %i\n", i, fds[i]);
            perror("Error getting file stats this is our own error");
            exit(1);
        }

        mapped_regions[i] = mmap(NULL, temp.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (mapped_regions[i] == MAP_FAILED) {
            printf("this failed SDFJDAKLSFJDASLKFJ\n");
            exit(EXIT_FAILURE);
        }
    }

  // Debug: Print disk images
  printf("Disk images:\n");
  for (int i = 0; i < num_disks; i++) {
    printf("  Disk %d: %s\n", i + 1, argv[i + 1]);
  }

  // Create a new array for FUSE arguments
  int fuse_argc = argc - num_disks;
  char **fuse_argv = malloc(fuse_argc * sizeof(char *));
  if (!fuse_argv) {
    // perror("Error allocating memory for FUSE arguments");
    return -1;
  }

  // Copy FUSE-related arguments to fuse_argv (skip argv[0] and disk image paths)
  fuse_argv[0] = argv[0];
  for (int i = num_disks + 1; i < argc; i++) {
    fuse_argv[i - num_disks] = argv[i];
  }

  // Debug: Print updated argc and argv for FUSE
  printf("FUSE argc: %d\n", fuse_argc);
  printf("FUSE argv:\n");
  for (int i = 0; i < fuse_argc; i++) {
    printf("  argv[%d]: %s\n", i, fuse_argv[i]);
  }

  return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}