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

int wfs_mkdir(const char *path, mode_t mode) {
    struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
    struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
    size_t num_inodes = superblock->num_inodes;

    // Find a free inode
    struct wfs_inode *new_inode = NULL;
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
    struct wfs_inode *parent_inode = locate_inode("/"); // Assuming root for simplicity
    if (!parent_inode) {
        return -ENOENT;
    }

    struct wfs_dentry *dentry_table = (struct wfs_dentry *)((char *)disk_images[0] + parent_inode->blocks[0]);
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

static const char *usage = "Usage: ./wfs disk1 disk2 [FUSE options] mount_point\n";

int main(int argc, char *argv[]) {
    // Ensure there are at least two disk images and one mount point.
    if (argc < 4) {
        fprintf(stderr, "Error: Too few arguments\n");
        fprintf(stderr, "%s", usage);
        return 1;
    }

    // Check for RAID argument passed during mkfs
    if (strcmp(argv[1], "-r") != 0) {
        fprintf(stderr, "Error: No RAID mode specified.\n");
        return -1;
    }

    // Determine RAID mode (0, 1, or 1v)
    raid_mode = -1;
    if (strcmp(argv[2], "0") == 0) {
        raid_mode = 0;
    } else if (strcmp(argv[2], "1") == 0) {
        raid_mode = 1;
    } else if (strcmp(argv[2], "1v") == 0) {
        raid_mode = 2;
    } else {
        fprintf(stderr, "Error: Invalid RAID mode.\n");
        return -1;
    }

    // Parse the disk images from argv (starting from argv[3] onwards)
    int num_disks = 0;
    while (argv[3 + num_disks] && argv[3 + num_disks][0] != '-') {
        num_disks++;
    }

    // At least two disks are required
    if (num_disks < 2) {
        fprintf(stderr, "Error: Not enough disks. At least two disks are required.\n");
        return -1;
    }

    // // The last argument is the mount point
    // const char *mount_point = argv[3 + num_disks];
    
    // Set up disk images (i.e., the array of paths to disk images)
    *disk_images[num_disks];
    for (int i = 0; i < num_disks; i++) {
        disk_images[i] = argv[3 + i];
    }

    // // Initialize FUSE options
    // struct fuse_operations ops = {
    //     .getattr = wfs_getattr,
    //     .mknod = wfs_mknod,
    //     .mkdir = wfs_mkdir,
    //     .unlink = wfs_unlink,
    //     .rmdir = wfs_rmdir,
    //     .read = wfs_read,
    //     .write = wfs_write,
    //     .readdir = wfs_readdir,
    // };

    // Set up FUSE arguments (remove disk images and RAID argument, keep the rest)
    char *fuse_args[argc - num_disks - 2];
    int fuse_arg_count = 0;

    for (int i = 1 + num_disks; i < argc - 1; i++) {
        fuse_args[fuse_arg_count++] = argv[i];
    }
    fuse_args[fuse_arg_count] = NULL;  // Null-terminate the arguments

    // Initialize the filesystem by calling fuse_main
    // This will mount the filesystem and enter the FUSE loop
    int ret = fuse_main(fuse_arg_count, fuse_args, &ops, NULL);

    return ret;
}
