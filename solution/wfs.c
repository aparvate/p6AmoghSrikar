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
#include <libgen.h>

int num_disks;
int raid_mode;
int fileDescs;
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
    return -1;
  }
  return superblock->d_blocks_ptr + BLOCK_SIZE * num;
}

struct wfs_inode *allocate_inode() {
  struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  off_t num = allocate_block((uint32_t *)((char *)disk_images[0] + superblock->d_bitmap_ptr), superblock->num_inodes / 32);
  if (num < 0) {
    return NULL;
  }
  struct wfs_inode *inode = (struct wfs_inode *)(((char *)disk_images[0] + superblock->d_bitmap_ptr) + num * BLOCK_SIZE);
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

  printf("Inode Starting\n");
  printf("Path: %s\n", path);
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

struct wfs_inode *find_inode_from_num (int num) {

    printf("find inode from num starting\n");

    struct wfs_sb *superblock = (struct wfs_sb *) disk_images[0];
    printf("checkpoint 1\n");
    int bits = 32;

    // Access the inode bitmap directly using the offset
    uint32_t *inode_bitmap = (uint32_t *)((char *) disk_images[0] + superblock->i_bitmap_ptr);

    printf("checkpoint 2\n");

    int bit_idx = num % bits;
    int array_idx = num / bits;

    printf("checkpoint 3\n");

    if (!(inode_bitmap[array_idx] & (0x1 << bit_idx))) {
        printf("checkpoint 4\n");
        return NULL;
    }

    printf("checkpoint 5\n");

    char *inode_table = ((char *) disk_images[0]) + superblock->i_blocks_ptr;
    printf("checkpoint 6\n");
    printf("find inode from num finished\n");
    return (struct wfs_inode *)((num * BLOCK_SIZE) + inode_table);
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
  printf("Get attribute starting\n");
  printf("Path: %s\n", path);
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
  //struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
  //struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
  //size_t num_inodes = superblock->num_inodes;

  // Find a free inode
  printf("Allocating node\n");
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

static int wfs_mkdir(const char *path, mode_t mode) {
    struct wfs_inode* check = locate_inode(path);
    if (!check) return -EEXIST;

    printf("Starting mkdir\n");
    struct wfs_sb *superblock = (struct wfs_sb *)disk_images[0];
    struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_images[0] + superblock->i_blocks_ptr);
    size_t num_inodes = superblock->num_inodes;
    printf("Superblock set, table set, num_inodes set\n");
    
    char path_copy[MAX_NAME], dir_copy[MAX_NAME];
    strncpy(path_copy, path, MAX_NAME-1);
    strncpy(dir_copy, path, MAX_NAME-1);
    path_copy[MAX_NAME-1] = dir_copy[MAX_NAME-1] = '\0';
    
    char *dir_name = basename(dir_copy);
    char *parent_path = dirname(path_copy);
    
    struct wfs_inode* parent_inode_num = locate_inode(parent_path);
    if (!parent_inode_num) return -ENOENT; // Parent directory not found

    int new_inode_num;
    for (size_t i = 0; i < num_inodes; i++) {
      printf("inode: %zd \n", i);
      if (inode_table[i].nlinks == 0) {
        new_inode_num = i;
        break;
      }
    }
    // int new_inode_num = get_free_inode();
    if (new_inode_num < 0) return -ENOSPC;
    
    struct wfs_inode new_dir = {0};
    new_dir.num = new_inode_num;
    new_dir.mode = S_IFDIR | (mode & 0777);
    new_dir.uid = getuid();
    new_dir.gid = getgid();
    new_dir.nlinks = 2;
    new_dir.atim = new_dir.mtim = new_dir.ctim = time(NULL);
    
    struct wfs_inode parent = {0};
    memcpy(&parent, parent_inode_num, sizeof(struct wfs_inode));
    
    if (parent.blocks[0] == 0 && parent.size == 0) {
        int block_num = allocate_DB();
        // if (block_num < 0) {
        //     free_inode(new_inode_num);
        //     return -ENOSPC;
        // }
        parent.blocks[0] = block_num;
        
        char zero_block[BLOCK_SIZE] = {0};
        for (int disk = 0; disk < num_disks; disk++) {
            memcpy((char*)disk_images[disk] + superblock->d_blocks_ptr + block_num * BLOCK_SIZE,
                   zero_block, BLOCK_SIZE);
        }
    }

    struct wfs_dentry entry = {0};
    strncpy(entry.name, dir_name, MAX_NAME - 1);
    entry.num = new_inode_num;

    parent.size += sizeof(struct wfs_dentry);
    parent.nlinks++;

    for (int disk = 0; disk < num_disks; disk++) {
        memcpy((char*)disk_images[disk] + superblock->i_blocks_ptr + new_inode_num * BLOCK_SIZE,
               &new_dir, sizeof(struct wfs_inode));
               
        memcpy((char*)disk_images[disk] + superblock->i_blocks_ptr + parent_inode_num->num * BLOCK_SIZE,
               &parent, sizeof(struct wfs_inode));
               
        memcpy((char*)disk_images[disk] + superblock->d_blocks_ptr + parent.blocks[0] * BLOCK_SIZE + parent.size - sizeof(struct wfs_dentry),
               &entry, sizeof(struct wfs_dentry));
               
        msync(disk_images[disk], superblock->i_blocks_ptr + (new_inode_num + 1) * BLOCK_SIZE, MS_SYNC);
        msync(disk_images[disk], superblock->d_blocks_ptr + (parent.blocks[0] + 1) * BLOCK_SIZE, MS_SYNC);
    }
    
    return 0;
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
  num_disks = 0;
  while (num_disks + 1 < argc && access(argv[num_disks + 1], F_OK) == 0)
  {
    num_disks++;
  }
  
  if (num_disks < 1) {
    fprintf(stderr, "Need at least 1 disks\n");
    return -1;
  }

  disks = malloc(sizeof(void *) * num_disks);
  if (disks == NULL) {
    fprintf(stderr, "Memory allocation failed for disks\n");
    return -1;
  }

  fileDescs = malloc(sizeof(int) * num_disks);
  if (fileDescs == NULL) {
    fprintf(stderr, "Memory allocation failed for fileDescs\n");
    return -1;
  }

  for (int i = 0; i < num_disks; i++) {
    fileDescs[i] = open(argv[i + 1], O_RDWR);
    if (fileDescs[i] == -1) {
      fprintf(stderr, "Failed to open disk %s\n", argv[i + 1]);
      return -1;
    }

    struct stat st;
    if (fstat(fileDescs[i], &st) != 0) {
      fprintf(stderr, "Failed to get disk size for %s\n", argv[i + 1]);
      return -1;
    }
    diskSize = st.st_size;

    disks[i] = mmap(NULL, diskSize, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescs[i], 0);
    if (disks[i] == MAP_FAILED) {
      fprintf(stderr, "Failed to mmap disk %s\n", argv[i + 1]);
      return -1;
    }
  }

  superblock = (struct wfs_sb *)disks[0];

  if (superblock == NULL) {
    fprintf(stderr, "Failed to access superblock\n");
    return -1;
  }

  num_disks = superblock->num_disks;
  //raid_mode = superblock->mode;

  int f_argc = argc - num_disks;
  char **f_argv = argv + num_disks;

  printf("f_argc: %d\n", f_argc);
  for (int i = 0; i < f_argc; i++) {
    printf("f_argv[%d]: %s\n", i, f_argv[i]);
  }

  int rc = fuse_main(f_argc, f_argv, &ops, NULL);
  printf("Returned from fuse\n");

  for (int i = 0; i < num_disks; i++) {
    if (munmap(disks[i], diskSize) != 0) {
      fprintf(stderr, "Failed to unmap disk %d\n", i);
      return -1;
    }
    close(fileDescs[i]);
  }

  free(disks);
  free(fileDescs);

  return rc;
}
