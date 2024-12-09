
#define FUSE_USE_VERSION 30
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/types.h"
#include "sys/mman.h"
#include "time.h"
#include "getopt.h"
#include "fuse.h"
#include "errno.h"
#include "wfs.h"
#include "stdbool.h"

#define SUCCEED 0
#define ERROR -1

void *disks[10];  // Disk images
struct wfs_sb *superblock;
int diskNum;
size_t diskSize;
static int *fileDescs;

struct wfs_dentry* get_dentry(void* disk, off_t block) {
    return (struct wfs_dentry *)((char *)disk + block);
}

char* get_bitmap(void* disk, off_t block) {
    return (char *)((char *)disk + block);
}

struct wfs_inode *get_inode(const char *path, char* disk) {
    // Start at the root inode
    char *inode_table = disk + superblock->i_blocks_ptr;
    struct wfs_inode *inode = (struct wfs_inode *)(inode_table);

    if (strcmp(path, "/") == 0) {
        return inode;  // Root directory
    }

    // Parse the path
    char temp_path[1024];
    strncpy(temp_path, path, sizeof(temp_path));
    char *token = strtok(temp_path, "/");
    while (token) {
        int found = 0;
        for (int i = 0; i < D_BLOCK; i++) {
            if (inode->blocks[i] == 0) break; // No more blocks

            struct wfs_dentry *dentry = get_dentry(disks[0], inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (strcmp(dentry[j].name, token) == 0) {
		   

                    inode = (struct wfs_inode *)(inode_table + (dentry[j].num * BLOCK_SIZE));
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }

        if (!found) {
            return NULL;  // Path does not exist
        }
        token = strtok(NULL, "/");
    }
    return inode;
}

int allocate_inode(char *disk) {
    if (superblock->raid_mode == 0) { // RAID 0 Mode
        int is_free = 1;  // Inode starts free
        for (int i = 0; i < superblock->num_inodes; i++) {
            is_free = 1;

            // Check all disks for the same inode bit
            for (int d = 0; d < diskNum; d++) {
                char *inode_bitmap = get_bitmap(disks[d], superblock->i_bitmap_ptr);
                if (inode_bitmap[i / 8] & (1 << (i % 8))) { // If allocated on any disk
                    is_free = 0; // Mark as not free
                    break;
                }
            }

            if (is_free) { // If the inode is free on all disks
                // Allocate the inode on all disks
                for (int d = 0; d < diskNum; d++) {
                    char *inode_bitmap = get_bitmap(disks[d], superblock->i_bitmap_ptr);
                    inode_bitmap[i / 8] |= (1 << (i % 8)); // Mark allocated
                }
                return i; // Return the inode number
            }
        }
    } else {
        // Standard (non-RAID 0) Mode
        char *inode_bitmap = get_bitmap((void*)disk, superblock->i_bitmap_ptr);
        for (int i = 0; i < superblock->num_inodes; i++) {
            if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) { // Free inode
                inode_bitmap[i / 8] |= (1 << (i % 8));    // Mark allocated
                return i;
            }
        }
    }

    return -ENOSPC; // No free inodes
}

int allocate_block(char *disk) {
    printf("RAID MODE: %d\n", superblock->raid_mode);
    // RAID 0
    if (superblock->raid_mode == 0) {
        for (int i = 0; i < superblock->num_data_blocks; i++) {
            uint8_t *currBitmap = (uint8_t *)disks[i % diskNum] + superblock->d_bitmap_ptr;

            if (!(currBitmap[((i / diskNum) / 8)] & (1 << ((i / diskNum) % 8)))) {
                currBitmap[((i / diskNum) / 8)] |= (1 << ((i / diskNum) % 8));

                // Zeroing out the newly allocated block
                char *blockPointer = (char *)disks[i % diskNum] + superblock->d_blocks_ptr + ((i / diskNum) * BLOCK_SIZE);
                char newBlock[BLOCK_SIZE];
                memset(newBlock, 0, BLOCK_SIZE);
                if (newBlock[0] != 0){
                    return ERROR;
                }
                memcpy(blockPointer, newBlock, BLOCK_SIZE);
                return i; // Return the logical block number
            }
        }
    } else { // RAID 1
        char *bitmap = get_bitmap((void*)disk, superblock->d_bitmap_ptr);
        for (int i = 0; i < superblock->num_data_blocks; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                return i;
            }
        }
    }

    return -ENOSPC; // No free blocks
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));

    struct wfs_inode *inode = get_inode(path, (char *)disks[0]);
    if (!inode) {
        return -ENOENT;
    }

    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    return SUCCEED;
}


// Parse the path into parent directory path and new directory name
static void parse_path(const char *path, char *dir_path, char *new, size_t size) {
    strncpy(dir_path, path, size - 1);
    dir_path[size - 1] = '\0';
    if (!strrchr(dir_path, '/') || strrchr(dir_path, '/') == dir_path) {
        strncpy(new, path + 1, MAX_NAME - 1); // Root directory
        new[MAX_NAME - 1] = '\0';
        strcpy(dir_path, "/");
    } else {
        strncpy(new, strrchr(dir_path, '/') + 1, MAX_NAME - 1);
        new[MAX_NAME - 1] = '\0';
        *strrchr(dir_path, '/') = '\0';
    }
}

// Initialize a new inode for the directory
static void initialize_new_inode(struct wfs_inode *inode, int inode_index, mode_t mode, bool nod) {
    memset(inode, 0, BLOCK_SIZE);
    inode->num = inode_index;
    if (nod){
        inode->mode = S_IFREG | mode; // Regular file mode
        inode->nlinks = 1;           // Single link
    }
    else{
        inode->mode = S_IFDIR | mode;
        inode->nlinks = 2;
    }
    inode->size = 0;
    inode->atim = inode->mtim = inode->ctim = time(NULL);
    inode->uid = getuid();
    inode->gid = getgid();
}

// Allocate and mirror the new inode across disks in RAID 0
static void mirror_inode_raid0(int inode_index, mode_t mode, bool nod) {
    for (int i = 0; i < diskNum; i++) {
        char *inode_table = (char *)disks[i] + superblock->i_blocks_ptr;
        struct wfs_inode *inode = (struct wfs_inode *)(inode_table + inode_index * BLOCK_SIZE);
        if (nod){
            initialize_new_inode(inode, inode_index, mode, true);
        }
        else{
            initialize_new_inode(inode, inode_index, mode, false);
        }
    }
}

static int add_directory_entry(struct wfs_inode *parent_inode, const char *parent_path, const char *new, int newIndex, char *disk) {
    int returnValue = 0;
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] == 0) {
            int blockIndex = allocate_block(disk);
            if (blockIndex < 0) 
                return -ENOSPC;
            if (superblock->raid_mode == 0) {
                int inBlock = blockIndex / diskNum;
                for (int d = 0; d < diskNum; d++) {
                    struct wfs_inode *sync_inode = get_inode(parent_path, (char *)disks[d]);
                    if (!sync_inode) {
                        return -ENOENT; // Parent inode missing in RAID 0 disk
                    }
                    sync_inode->blocks[i] = superblock->d_blocks_ptr + inBlock * BLOCK_SIZE;
                }
            } 
            else {
                parent_inode->blocks[i] = superblock->d_blocks_ptr + blockIndex * BLOCK_SIZE;
            }
        }
        struct wfs_dentry *dentry = get_dentry((void *)disk, parent_inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dentry[j].num == 0) {
                strncpy(dentry[j].name, new, MAX_NAME - 1);
                dentry[j].name[MAX_NAME - 1] = '\0';
                dentry[j].num = newIndex;
                parent_inode->size += sizeof(struct wfs_dentry);
                returnValue = 1;
                break;
            }
        }
        if (returnValue)
            break;
    }
    return returnValue ? SUCCEED : -ENOSPC;
}

static int add_file_entry(struct wfs_inode *parent_inode, const char *parent_path, const char *new, int newIndex, char *disk) {
    int returnValue = 0;
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] == 0) {
            int blockIndex = allocate_block(disk);
            if (blockIndex < 0) 
                return -ENOSPC;
            parent_inode->blocks[i] = superblock->d_blocks_ptr + blockIndex * BLOCK_SIZE;
        }

        struct wfs_dentry *dentry = get_dentry((void *)disk, parent_inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dentry[j].num == 0) {
                strncpy(dentry[j].name, new, MAX_NAME - 1);
                dentry[j].name[MAX_NAME - 1] = '\0';
                dentry[j].num = newIndex;
                parent_inode->size += sizeof(struct wfs_dentry);
                returnValue = 1;
                break;
            }
        }
        if (returnValue) {
            break;
        }
    }

    return returnValue ? SUCCEED : -ENOSPC; // Return success or error if no space found
}

static int wfs_mkdir_helper(const char *path, mode_t mode, char *disk) {
    printf("mkdir helper\n");

    char parent_path[1024], new_name[MAX_NAME];
    parse_path(path, parent_path, new_name, sizeof(parent_path));

    struct wfs_inode *parent_inode = get_inode(parent_path, disk);
    if (!parent_inode) {
        //printf("Dir does not exist");
        return -ENOENT;
    }

    int new_inode_index = allocate_inode(disk);
    if (new_inode_index < 0) {
        //printf("Node does not exist");
        return new_inode_index; // Propagate ENOSPC
    }

    printf("Node %d\n", new_inode_index);
    if (superblock->raid_mode == 0) {
        //printf("Raid Mode 0");
        mirror_inode_raid0(new_inode_index, mode, false);
    } else {
        //printf("Raid Mode 1");
        char *inode_table = disk + superblock->i_blocks_ptr;
        struct wfs_inode *new_inode = (struct wfs_inode *)(inode_table + new_inode_index * BLOCK_SIZE);
        initialize_new_inode(new_inode, new_inode_index, mode, false);
    }
    int result = add_directory_entry(parent_inode, parent_path, new_name, new_inode_index, disk);
    return result;
}

// FUSE mkdir implementation
static int wfs_mkdir(const char *path, mode_t mode) {
    if (superblock->raid_mode == 0) {
        return wfs_mkdir_helper(path, mode, (char *)disks[0]);
    } else {
        for (int i = 0; i < diskNum; i++) {
            int result = wfs_mkdir_helper(path, mode, (char *)disks[i]);
            if (result < 0) {
                return result;
            }
        }
    }
    return SUCCEED;
}

// Helper function for wfs_mknod
static int wfs_mknod_helper(const char *path, mode_t mode, char *disk) {
    printf("mknod helper\n");

    char parent_path[1024], new_name[MAX_NAME];
    parse_path(path, parent_path, new_name, sizeof(parent_path));

    // Fetch the parent inode
    struct wfs_inode *parent_inode = get_inode(parent_path, disk);
    if (!parent_inode) {
        //printf("Dir does not exist");
        return -ENOENT;
    }

    int new_inode_index = allocate_inode(disk);
    if (new_inode_index < 0) {
        //printf("Node does not exist");
        return new_inode_index; // Propagate ENOSPC
    }

    printf("Node %d\n", new_inode_index);
    if (superblock->raid_mode == 0) {
        //printf("Raid Mode 0");
        mirror_inode_raid0(new_inode_index, mode, true);
    } else {
        //printf("Raid Mode 1");
        char *inode_table = disk + superblock->i_blocks_ptr;
        struct wfs_inode *new_inode = (struct wfs_inode *)(inode_table + new_inode_index * BLOCK_SIZE);
        initialize_new_inode(new_inode, new_inode_index, mode, true);
    }

    // Add file entry to the parent directory
    int result = add_file_entry(parent_inode, parent_path, new_name, new_inode_index, disk);
    return result;
}

// FUSE mknod implementation
static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    int result = 0;

    if (superblock->raid_mode == 0) {
        result = wfs_mknod_helper(path, mode, (char *)disks[0]);
    } else {
        for (int i = 0; i < diskNum; i++) {
            result = wfs_mknod_helper(path, mode, (char *)disks[i]);
            if (result < 0) {
                for (int j = 0; j <= i; j++) {
                    char *disk = (char *)disks[j];
                    char *inode_bitmap = disk + superblock->i_bitmap_ptr;
                    int inode_num = allocate_inode(disk);
                    if (inode_num >= 0) {
                        inode_bitmap[inode_num / 8] &= ~(1 << (inode_num % 8));
                    }
                }
                return result;
            }
        }
    }

    return result;
}

static void fill_directory_entries(void *buf, fuse_fill_dir_t filler, char *disk, uint32_t block) {
    struct wfs_dentry *dentry = get_dentry(disk, block);

    for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
        if (dentry[j].num != 0) { // Valid entry
            filler(buf, dentry[j].name, NULL, 0);
            printf("Adding entry number %s\n", dentry[j].name);
        }
    }
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("%s must be read\n", path);
    filler(buf, ".", NULL, 0);  // Current directory
    filler(buf, "..", NULL, 0); // Parent directory
    struct wfs_inode *inode = get_inode(path, (char*)disks[0]);
    if (!inode || !(inode->mode & S_IFDIR))
        return -ENOENT;
    for (int i = 0; i < D_BLOCK; i++) {
        if (inode->blocks[i] == 0) {
            break;
        }

        fill_directory_entries(buf, filler, (char *)disks[0], inode->blocks[i]);
    }
    return SUCCEED;
}

static void *get_data_block(struct wfs_inode *inode, size_t block_offset) {
    if (block_offset < D_BLOCK) {
        if (inode->blocks[block_offset] == 0)
            return NULL;
        else
            return (char *)disks[0] + inode->blocks[block_offset];
    }
    if (inode->blocks[IND_BLOCK] == 0)
        return NULL;
    uint32_t *indirect_block = (uint32_t *)((char *)disks[0] + inode->blocks[IND_BLOCK]);
    if (indirect_block[block_offset - D_BLOCK] == 0)
        return NULL;

    return (char *)disks[0] + indirect_block[block_offset - D_BLOCK];
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode = get_inode(path, (char *)disks[0]);
    if (!inode) {
        return -ENOENT; // File not found
    }
    if ((inode->mode & S_IFDIR)) {
        printf("Cannot read\n");
        return -EISDIR; // Not a directory
    }

    size_t bytes_read = 0;
    size_t block_offset = offset / BLOCK_SIZE;
    size_t block_start_offset = offset % BLOCK_SIZE;

    while (bytes_read < size) {
        void *data_block = get_data_block(inode, block_offset);
        if (!data_block) 
            break;
        size_t block_available_space = BLOCK_SIZE - block_start_offset;
        size_t bytes_to_read = (size - bytes_read < block_available_space) ? size - bytes_read : block_available_space;
        memcpy(buf + bytes_read, (char *)data_block + block_start_offset, bytes_to_read);
        bytes_read += bytes_to_read;
        block_offset++;
        block_start_offset = 0;
    }

    return bytes_read;
}

static int allocate_and_map_direct_block(struct wfs_inode *inode, int block_offset, int disk_index, int logical_block_num) {
    int new_block = allocate_block((char *)disks[disk_index]);
    if (new_block < 0) {
        return -ENOSPC;
    }
    if (superblock->raid_mode == 0) {
        for (int i = 0; i < diskNum; i++) {
            char *disk = (char *)disks[i];
            struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
            mirror_inode->blocks[logical_block_num] = superblock->d_blocks_ptr + new_block * BLOCK_SIZE;
        }
    } else {
        for (int i = 0; i < diskNum; i++) {
            char *disk = (char *)disks[i];
            ((char*)(disk + superblock->d_bitmap_ptr))[new_block / 8] |= (1 << (new_block % 8));
            struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
            mirror_inode->blocks[block_offset] = superblock->d_blocks_ptr + new_block * BLOCK_SIZE;
        }
    }
    return 0;
}

static int allocate_and_map_indirect_block(struct wfs_inode *inode, int disk_index) {
    int indirect_block_index = allocate_block((char *)disks[disk_index]);
    if (indirect_block_index < 0) {
        return -ENOSPC; // No space available
    }
    if (superblock->raid_mode == 0) {
        for (int i = 0; i < diskNum; i++) {
            char *disk = (char *)disks[i];
            void *indirect_block_ptr = disk + superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
            memset(indirect_block_ptr, 0, BLOCK_SIZE);
            struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
            mirror_inode->blocks[IND_BLOCK] = superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
        }
    } else {
        for (int i = 0; i < diskNum; i++) {
            char *disk = (char *)disks[i];
            char *data_bitmap = disk + superblock->d_bitmap_ptr;
            data_bitmap[indirect_block_index / 8] |= (1 << (indirect_block_index % 8));
            void *indirect_block_ptr = disk + superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
            memset(indirect_block_ptr, 0, BLOCK_SIZE);
            struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
            mirror_inode->blocks[IND_BLOCK] = superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
        }
    }
    return 0;
}

static void write_to_block(const char *buf, size_t bytes_written, size_t bytes_to_write, size_t block_start_offset, uint32_t block_address, int disk_index) {
    if (superblock->raid_mode == 0) {
        void *block_ptr = (char *)disks[disk_index] + block_address + block_start_offset;
        memcpy(block_ptr, buf + bytes_written, bytes_to_write);
    } else {
        for (int i = 0; i < diskNum; i++) {
            char *disk = (char *)disks[i];
            void *block_ptr = (char *)disk + block_address + block_start_offset;
            memcpy(block_ptr, buf + bytes_written, bytes_to_write);
        }
    }
}

// Main `wfs_write` function
static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode = get_inode(path, (char *)disks[0]);
    if (!inode) {
        return -ENOENT; // File not found
    }
    if ((inode->mode & S_IFDIR)) {
        printf("Cannot read\n");
        return -EISDIR; // Not a directory
    }

    size_t bytes = 0;
    size_t bOff = offset / BLOCK_SIZE;
    size_t bStart = offset % BLOCK_SIZE;
    size_t bSpace;
    size_t bytesWrite;

    while (bytes < size) {
        int disk_index = (superblock->raid_mode == 0) ? bOff % diskNum : 0;
        int logical_block_num = (superblock->raid_mode == 0) ? bOff / diskNum : bOff;
        if (bOff < D_BLOCK) {
            if (inode->blocks[bOff] == 0) {
                if (allocate_and_map_direct_block(inode, bOff, disk_index, logical_block_num) < 0) {
                    return -ENOSPC;
                }
            }
            bSpace = BLOCK_SIZE - bStart;
            if (size - bytes < bSpace)
                bytesWrite = size - bytes;
            else
                bytesWrite = bSpace;

            write_to_block(buf, bytes, bytesWrite, bStart, inode->blocks[logical_block_num], disk_index);
        }
        else {
            size_t indirect_offset = bOff - D_BLOCK;
            if (inode->blocks[IND_BLOCK] == 0) {
                if (allocate_and_map_indirect_block(inode, disk_index) < 0) {
                    return -ENOSPC;
                }
            }
            uint32_t *indirect_block = (uint32_t *)((char *)disks[disk_index] + inode->blocks[IND_BLOCK]);
            if (indirect_block[indirect_offset] == 0) {
                if (allocate_and_map_direct_block(inode, indirect_offset, disk_index, logical_block_num) < 0) {
                    return -ENOSPC;
                }
            }
            bSpace = BLOCK_SIZE - bStart;
            if (size - bytes < bSpace)
                bytesWrite = size - bytes;
            else
                bytesWrite = bSpace;

            write_to_block(buf, bytes, bytesWrite, bStart, indirect_block[indirect_offset], disk_index);
        }
        bytes += bytesWrite;
        bOff++;
        bStart = 0; // Reset for subsequent blocks
    }

    // Update inode metadata on all disks
    for (int i = 0; i < diskNum; i++) {
        struct wfs_inode *mirror_inode = (struct wfs_inode *)((char *)disks[i] + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
        mirror_inode->size = (offset + size > mirror_inode->size) ? offset + size : mirror_inode->size;
        //mirror_inode->mtim = time(NULL);
    }

    return bytes;
}


static int wfs_unlink(const char *path) {
    printf("unlink called for path: %s\n", path);
    // int returnValue = 0;
    // for (int i = 0; i < D_BLOCK; i++) {
    //     if (parent_inode->blocks[i] == 0) {
    //         int blockIndex = allocate_block(disk);
    //         if (blockIndex < 0) 
    //             return -ENOSPC;
    //         parent_inode->blocks[i] = superblock->d_blocks_ptr + blockIndex * BLOCK_SIZE;
    //     }
    //TODO
    return SUCCEED; // SUCCEED
}

static int wfs_rmdir(const char *path) {
    printf("rmdir called for path: %s\n", path);
    //TODO
    // printf("rmdir\n");

    // char parent_path[1024], new_name[MAX_NAME];
    // parse_path(path, parent_path, new_name, sizeof(parent_path));

    // // Fetch the node
    // struct wfs_inode *parent_inode = get_inode(parent_path, disk);
    // if (!parent_inode) {
    //     //printf("Dir does not exist");
    //     return -ENOENT;
    // }
    return SUCCEED; // SUCCEED
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .readdir = wfs_readdir,
    .read = wfs_read,
    .write = wfs_write,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir
};

int main(int argc, char *argv[]) {
    diskNum = 0;
    while (diskNum + 1 < argc && access(argv[diskNum + 1], F_OK) == 0)
    {
        diskNum++;
    }
    if (diskNum < 1) {
        return ERROR;
    }

    fileDescs = malloc(sizeof(int) * diskNum);
    if (fileDescs == NULL) {
        return ERROR;
    }

    for (int i = 0; i < diskNum; i++) {
        fileDescs[i] = open(argv[i + 1], O_RDWR);
        if (fileDescs[i] == -1) {
            return ERROR;
        }

        struct stat st;
        if (fstat(fileDescs[i], &st) != 0) {
            return ERROR;
        }
        diskSize = st.st_size;

        disks[i] = mmap(NULL, diskSize, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescs[i], 0);
        if (disks[i] == MAP_FAILED) {
            return ERROR;
        }
    }

    superblock = (struct wfs_sb *)disks[0];
    if (superblock == NULL) {
        return ERROR;
    }

    diskNum = superblock->num_disks;
    int f_argc = argc - diskNum;
    char **f_argv = argv + diskNum;

    //printf("Argument: %d\n", f_argc);
    //for (int i = 0; i < f_argc; i++) {
        //printf("Argument number %d: %s\n", i, f_argv[i]);
    //}

    int returnValue = fuse_main(f_argc, f_argv, &ops, NULL);
    //printf("Returned from fuse\n");

    for (int i = 0; i < diskNum; i++) {
        if (munmap(disks[i], diskSize) != 0) {
            fprintf(stderr, "Failed to unmap disk %d\n", i);
            return ERROR;
        }
        close(fileDescs[i]);
    }

    //free(disks);
    free(fileDescs);

    return returnValue;
}

