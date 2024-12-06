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

struct wfs_inode *locate_inode(const char *path) {
  struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
  size_t num_inodes = superblock->num_inodes;

  for (size_t i = 0; i < num_inodes; i++) {
    struct wfs_inode *inode = &inode_table[i];
    if (inode->nlinks > 0) {
      struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)disk_images[0] + inode->blocks[0]);
      for (size_t j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
        if (strcmp(dentry_table[j].name, path) == 0) {
          return inode;
        }
      }
    }
  }

  return NULL; // fails
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
  memset(stbuf, 0, sizeof(struct stat));

  if (strcmp(path, "/") == 0) {
    // Root directory
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    return 0;
  }

  // Locate the file or directory in the inode table
  struct wfs_inode *inode = locate_inode(path);
  if (!inode) {
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

  return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
  printf("mknod called for path: %s\n", path);
  return 0;
}

int wfs_mkdir(const char path, mode_t mode) {
    struct wfs_sbsuperblock = (struct wfs_sb )disk_images[0];
    struct wfs_inodeinode_table = (struct wfs_inode )((char)disk_images[0] + superblock->i_blocks_ptr);
    size_t num_inodes = superblock->num_inodes;

    // Find a free inode
    struct wfs_inode new_inode = NULL;
    for (size_t i = 0; i < num_inodes; i++) {
        if (inode_table[i].nlinks == 0) {
            new_inode = &inode_table[i];
            break;
        }
    }

    if (!new_inode) {
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
    struct wfs_inodeparent_inode = locate_inode("/"); // Assuming root for simplicity
    if (!parent_inode) {
        return -ENOENT;
    }

    struct wfs_dentry dentry_table = (struct wfs_dentry)((char *)disk_images[0] + parent_inode->blocks[0]);
    for (size_t i = 0; i < BLOCK_SIZE / sizeof(struct wfs_dentry); i++) {
        if (dentry_table[i].num == 0) {
            strncpy(dentry_table[i].name, path, MAX_NAME);
            dentry_table[i].num = new_inode - inode_table; // Index of the new inode
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
  // Initialize FUSE with specified operations
  // Filter argc and argv here and then pass it to fuse_main
  return fuse_main(argc, argv, &ops, NULL);
}