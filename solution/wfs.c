#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "wfs.h"

int num_disks;
int raid_mode;
char *disks[10];
void *disk_images[10];

size_t allocate_block(uint32_t *bitmap, size_t size) {
  for (uint32_t i = 0; i < size; i++) {
    uint32_t bitmap_region = bitmap[i];
    uint32_t k = 0;
    while (bitmap_region != UINT32_MAX && k < 32) {
      if (!((bitmap_region >> k) & 0x1)) {
        bitmap[i] = bitmap[i] | (0x1 << k);
        return i * 32 + k;
      }
      k++;
    }
  }
  return -1;
}

off_t allocate_DB() {
  struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  off_t num = allocate_block((uint32_t *)((char *)disk_images[0] + superblock->d_bitmap_ptr), superblock->num_data_blocks / 32);
  if (num < 0) {
    err = -ENOSPC;  
    return -1;
  }
  return superblock->d_blocks_ptr + BLOCK_SIZE * num;
}

struct wfs_inode *allocate_inode() {
  struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  off_t num = allocate_block((uint32_t *)((char *)disk_images[0] + superblock->d_bitmap_ptr), superblock->num_inodes / 32);
  if (num < 0) {
    err = -ENOSPC;
    return NULL;
  }
  struct wfs_inode *inode = (struct wfs_inode *)((char *)mmap_ptr(superblock->i_blocks_ptr) + num * BLOCK_SIZE);
  inode->num = num;
  return inode;
}

struct wfs_inode *locate_inode(const char *path) {
    if (strcmp("/", path) == 0) {
        printf("Found root path\n");
        struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
        struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
        return &inode_table[0]; // Return the root inode
    }

    struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
    struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
    printf("Superblock and inode table initialized.\n");

    char path_copy[strlen(path) + 1];
    strcpy(path_copy, path);

    char *token = strtok(path_copy, "/");
    struct wfs_inode *current_inode = &inode_table[0]; // Start at the root inode

    while (token != NULL) {
        int found = 0;
        struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)disk_images[0] + current_inode->blocks[0]);

        for (size_t i = 0; i < BLOCK_SIZE / sizeof(struct wfs_dentry); i++) {
            if (dentry_table[i].num > 0 && strcmp(dentry_table[i].name, token) == 0) {
                current_inode = &inode_table[dentry_table[i].num];
                found = 1;
                break; // Stop searching once the directory entry is found
            }
        }

        if (!found) {
            printf("Path component '%s' not found.\n", token);
            return NULL; // Stop if the current token is not found
        }

        token = strtok(NULL, "/"); // Move to the next component in the path
    }

    printf("Inode found for path.\n");
    return current_inode;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
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
  struct wfs_inode *inode = locate_inode(path);
  printf("Found INODE\n");
  if (!inode) {
    printf("No INODE!\n");
    return -ENOENT;
  }

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

static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
  printf("Starting mknod\n");
  // struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  // struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
  // size_t num_inodes = superblock->num_inodes;
  // printf("Superblock set, table set, num_inodes set\n");

  // struct wfs_inode *new_inode = NULL;
  // for (size_t i = 0; i < num_inodes; i++) {
  //   printf("inode: %zd \n", i);
  //   if (inode_table[i].nlinks == 0) {
  //     new_inode = &inode_table[i];
  //     break;
  //   }
  // }

  // if (!new_inode) {
  //   printf("No INODE\n");
  //   return -ENOSPC;
  // }
  // // Initialize the new inode
  // memset(new_inode, 0, sizeof(struct wfs_inode));
  // new_inode->mode = S_IFDIR | mode;
  // new_inode->uid = getuid();
  // new_inode->gid = getgid();
  // new_inode->nlinks = 2;
  // new_inode->atim = time(NULL);
  // new_inode->mtim = time(NULL);
  // new_inode->ctim = time(NULL);

  // printf("mknod called for path: %s\n", path);
  return 0;
}

int wfs_mkdir(const char *path, mode_t mode) {
  printf("Starting mkdir\n");
  struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
  size_t num_inodes = superblock->num_inodes;
  printf("Superblock set, table set, num_inodes set\n");

  // Find a free inode
  struct wfs_inode *new_inode = allocate_inode();

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
  new_inode->num = 12;

  // Find a free directory entry
  struct wfs_inode *parent_inode = locate_inode("/");
  if (!parent_inode) {
    printf("Root INODE not found");
    return -ENOENT;
  }

  struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)disk_images[0] + parent_inode->blocks[0]);
  for (size_t i = 0; i < BLOCK_SIZE / sizeof(struct wfs_dentry); i++) {
    printf("dentry: %zd \n", i);
    if (dentry_table[i].num == 0) {
      printf("Creating new directory");
      strncpy(dentry_table[i].name, path, MAX_NAME);
      dentry_table[i].num = new_inode->num;
      parent_inode->nlinks++;
      return 0;
    }
  }

  return -ENOSPC;
}

static int wfs_unlink(const char *path) {
  printf("unlink called for path: %s\n", path);
  return 0;
}

static int wfs_rmdir(const char *path) {
  printf("rmdir called for path: %s\n", path);
  return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  printf("read called for path: %s\n", path);
  return 0;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  printf("write called for path: %s\n", path);
  return size;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
  printf("readdir called for path: %s\n", path);
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

  printf("Hawk Tuah!\n");
  for (int i = 0; i < argc; i++) {
    printf("  argv[%d]: %s\n", i, argv[i]);
  }

  // Identify disk image arguments
  num_disks = 0;
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
  int fuse_argc = argc - num_disks + 1; // Exclude disk images and argv[0], add space for 1 null ending
  char **fuse_argv = malloc(fuse_argc * sizeof(char *));
  if (!fuse_argv) {
    perror("Error allocating memory for FUSE arguments");
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
  fuse_argc = fuse_argc - 1;

  // Start FUSE
  int result = fuse_main(fuse_argc, fuse_argv, &ops, NULL);
  printf("Fuse main passed\n");

  // Free allocated memory
  free(fuse_argv);
  printf("Args freed\n");

  return result;
}