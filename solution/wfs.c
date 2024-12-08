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

#define MAX_PATH_NAME 264
#define MAX_DISKS 16
#define SUCCESS 0
#define FAIL -1

// Global variables for filesystem
static char **disks;
static struct wfs_sb *superblock;
static int num_disks;
static int *fileDescs;
size_t diskSize;
//static char* blocks;

void split_path(const char *path, char *parent_path, char *new_name) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        strcpy(parent_path, "/");
        strcpy(new_name, last_slash ? last_slash + 1 : path);
    }
    else{
        size_t parent_len = last_slash - path;
        strncpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        strcpy(new_name, last_slash + 1);
    }
}

char* get_inode_block_addr(int disk_idx, int inode_idx) {
    return (char *) (disks[disk_idx]) + (superblock->i_blocks_ptr) + (inode_idx * BLOCK_SIZE);
}

char* get_data_block_addr(int disk_idx, int inode_idx, off_t block_idx) {
    return (char *) (disks[disk_idx]) + (superblock->d_blocks_ptr) + (inode_idx * (superblock->num_data_blocks / superblock->num_inodes) * BLOCK_SIZE) + (block_idx * BLOCK_SIZE);
}

int raid_read_block(int disk_idx, int inode_idx, off_t block_idx, char *data, size_t size) {
    if (superblock->mode == 0) {
        printf("raid_read_block: reading for mode 0\n");

        printf("raid_read_block: TODO\n");
        return FAIL;
    } 
    else if (superblock->mode == 1) {
        printf("raid_read_block: reading for mode 1\n");

        memcpy(data, get_data_block_addr(disk_idx, inode_idx, block_idx), size);

        printf("raid_read_block: finished\n");
    } 
    else if (superblock->mode == 2) {
        printf("raid_read_block: reading for mode 2\n");

        printf("raid_read_block: TODO\n");
        return FAIL;
    }
    return SUCCESS;
}

static void* get_disk_block(int block_idx, int disk_idx) {
    return (char *) disks[disk_idx] + block_idx * BLOCK_SIZE;
}

static int get_raid_disk(int block_idx) {
    return superblock -> mode == 0 ? block_idx % num_disks : 0;  // RAID 0 or mirrored
}

static void* get_block(int block_idx) {

    int disk_idx = get_raid_disk(block_idx);
    int local_block_idx = superblock -> mode == 0 ? block_idx / num_disks : block_idx;
    return get_disk_block(local_block_idx, disk_idx);
}

struct wfs_inode* get_inode(off_t index) {
    //printf("Entering get_inode\n");
    // Validate the inode index
    if (index < 0 || index >= superblock->num_inodes) {
        printf("get_inode: failed\n");
        return NULL;
    }

    // Calculate the offset for the inode in the inode block region
    char* inode_offset = (char*) disks[0] + superblock->i_blocks_ptr; 
    //printf("offset- %s\n", inode_offset);
    //printf("disk ptr- ");
    //printf("disk + offset: %li\n", (off_t) disks[0] + inode_offset);

    // In RAID 0 and RAID 1 modes, we'll always use the first disk
    printf("Returing from get_inode\n");
    return (struct wfs_inode*)((char*)inode_offset + index * BLOCK_SIZE);
}

static int allocate_data_block() {
    printf("Entering allocate_data_block\n");
    for (int i = 0; i < superblock->num_data_blocks; i++) {
        // Track if the block is free across all disks
        int is_free = 1;
        
        // Debug: Print which block we're checking
        printf("Checking block %d\n", i);
        
        // Check bitmap for each disk
        for (int j = 0; j < superblock->num_disks; j++) {
            char *data_bitmap = (char*)disks[j] + superblock->d_bitmap_ptr;
            
            // Debug: Print bitmap information
            int is_used = (data_bitmap[i / 8] & (1 << (i % 8))) != 0;
            printf("Disk %d, Block %d: Used = %d\n", j, i, is_used);
            
            if (is_used) {
                is_free = 0;
                break;
            }
        }
        
        if (is_free) {
            printf("Allocating block %d\n", i);
            
            // Mark block as used on ALL disks
            for (int disk = 0; disk < superblock->num_disks; disk++) {
                char *data_bitmap = (char*)disks[disk] + superblock->d_bitmap_ptr;
                data_bitmap[i / 8] |= (1 << (i % 8));
                
                // Debug: Confirm bitmap update
                printf("Marked block %d as used on disk %d\n", i, disk);
            }
            
            return i;
        }
    }
    
    printf("No free blocks found\n");
    return -ENOSPC;  // No space left
}

static int allocate_inode() {
    //char *inode_bitmap = (char*)disks[0] + superblock->i_bitmap_ptr;
    
    for (int i = 0; i < superblock->num_inodes; i++) {
        // Check if the inode is free on ALL disks
        int is_free = 1;
        for (int j = 0; j < superblock->num_disks; j++) {
            char *disk_bitmap = (char*)disks[j] + superblock->i_bitmap_ptr;
            if (disk_bitmap[i / 8] & (1 << (i % 8))) {
                is_free = 0;
                break;
            }
        }
        
        if (is_free) {
            // Mark inode as used on ALL disks
            for (int disk = 0; disk < superblock->num_disks; disk++) {
                char *disk_bitmap = (char*)disks[disk] + superblock->i_bitmap_ptr;
                disk_bitmap[i / 8] |= (1 << (i % 8));
            }
            return i;
        }
    }
    
    return -ENOSPC;  // No space left
}

// static int add_parent_dir_entry(off_t parentIdx, const char *name, off_t newIdx) {
//     struct wfs_inode *parentInode = get_inode(parentIdx);
    
//     // Calculate how many entries are currently in the directory
//     size_t current_entries = parentInode->size / sizeof(struct wfs_dentry);
//     size_t entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);
    
//     // Calculate which block we need and the offset within that block
//     size_t block_idx = current_entries / entries_per_block;
//     size_t entry_offset = current_entries % entries_per_block;
    
//     // Check if we need a new block
//     if (block_idx >= N_BLOCKS) {
//         return -ENOSPC;  // No more blocks available
//     }
    
//     // Allocate new block if needed
//     bool is_new_block = false;
//     if (parentInode->blocks[block_idx] == 0) {
//         int newBlock = allocate_data_block();
//         if (newBlock < 0) {
//             return newBlock;
//         }
//         parentInode->blocks[block_idx] = newBlock;
//         is_new_block = true;
//     }
    
//     // Only zero out the block if it's newly allocated
//     if (is_new_block) {
//         for (int disk = 0; disk < superblock->num_disks; disk++) {
//             char *blockAddr = (char*)disks[disk] + superblock->d_blocks_ptr + 
//                             parentInode->blocks[block_idx] * BLOCK_SIZE;
//             memset(blockAddr, 0, BLOCK_SIZE);
//         }
//     }
    
//     // Create the new directory entry
//     struct wfs_dentry newEntry;
//     strncpy(newEntry.name, name, MAX_NAME - 1);
//     newEntry.name[MAX_NAME - 1] = '\0';
//     newEntry.num = newIdx;
    
//     // Write the new entry to all disks
//     for (int disk = 0; disk < superblock->num_disks; disk++) {
//         // Calculate entry position in this disk
//         char *blockAddr = (char*)disks[disk] + superblock->d_blocks_ptr + 
//                          parentInode->blocks[block_idx] * BLOCK_SIZE;
//         struct wfs_dentry *entries = (struct wfs_dentry*)blockAddr;
        
//         // Write the new entry at the correct offset without disturbing existing entries
//         memcpy(&entries[entry_offset], &newEntry, sizeof(struct wfs_dentry));
//     }
    
//     // Update parent inode
//     parentInode->size += sizeof(struct wfs_dentry);
//     parentInode->nlinks++;
    
//     // Write updated inode to all disks
//     for (int disk = 0; disk < superblock->num_disks; disk++) {
//         struct wfs_inode *diskInode = (struct wfs_inode*)
//             ((char*)disks[disk] + superblock->i_blocks_ptr + parentIdx * BLOCK_SIZE);
//         memcpy(diskInode, parentInode, sizeof(struct wfs_inode));
//         msync(disks[disk], diskSize, MS_SYNC);
//     }
    
//     return 0;
// }

// static void write_inode(off_t idx, struct wfs_inode* newInode){
//     printf("Entering write_inode\n");
//     struct wfs_inode *inodeAddr = (struct wfs_inode*) get_inode_block_addr(0, idx);
//     memcpy(inodeAddr, newInode, BLOCK_SIZE);
//     printf("Exiting write_inode\n");
//     return;
// }

// static void write_inode_across_disks(off_t idx, struct wfs_inode* newInode) {
//     printf("Entering write_inode_across_disks\n");
    
//     // Write the inode to the inode block on ALL disks
//     for (int disk = 0; disk < superblock->num_disks; disk++) {
//         // Calculate the inode address for this specific disk
//         struct wfs_inode *inodeAddr = (struct wfs_inode*)
//             ((char*)disks[disk] + superblock->i_blocks_ptr + idx * BLOCK_SIZE);
        
//         // Copy the entire inode block
//         memcpy(inodeAddr, newInode, BLOCK_SIZE);
//     }
    
//     printf("Exiting write_inode_across_disks\n");
// }

static off_t find_inode(const char *path) {
    printf("Entering find_inode: path = %s\n", path);
    // If the path is root, return the root inode index (0).
    if (strcmp(path, "/") == 0) {
        printf("find_inode: path is root\n");
        return 0;
    }

    //printf("%s\n", path);
    // Copy the path for tokenization (strtok modifies the string).
    char temp_path[MAX_PATH_NAME];
    strncpy(temp_path, path, MAX_PATH_NAME-1);
    char *component = temp_path[0] == '/' ? temp_path + 1 : temp_path;

    // Tokenize the path into components (e.g., "dir1", "dir2", "file").
    char *token = strtok(component, "/");

    int current_inode = 0; // Start at the root inode

    while (token != NULL) {
        struct wfs_inode *dir_inode;
        dir_inode = get_inode(current_inode);

        // Ensure the current inode is a directory.
        if (!(dir_inode->mode & S_IFDIR)) {
            printf("Not a directory\n");
            return -ENOTDIR; // Not a directory
        }

        // Search for the token (path component) in the directory's entries.
        int found = 0;

        for (int offset = 0; offset < dir_inode->size; offset += sizeof(struct wfs_dentry)) {
            struct wfs_dentry *entry = (struct wfs_dentry *)(disks[0] + superblock->d_blocks_ptr + 
                                       dir_inode->blocks[0] * BLOCK_SIZE + offset);
            
            if (strcmp(entry->name, token) == 0) {
                current_inode = entry->num;
                found = 1;
                break;
            }
        }

        // If not found, return error
        if (!found) {
            printf("Find inode exiting did not find\n");
            return -ENOENT;
        }
        

        // Proceed to the next component in the path.
        token = strtok(NULL, "/");
    }
    printf("find_inode: Successfully returning inode %i\n", current_inode);
    return current_inode; // Return the resolved inode index
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    printf("Entering wfs_getattr: Path = %s\n", path);

    memset(stbuf, 0, sizeof(struct stat));

    int inode_idx = find_inode(path);
   // printf("Inode idx: %i\n", inode_idx);
    if (inode_idx < 0) {
        fprintf(stderr, "wfs_getattr: Found invalid inode index\n");
        return inode_idx;
    }
    struct wfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "wfs_getattr: Invalid inode index, cannot find inode\n");
        return -ENOENT;
    }
    //printf("wfs_getattr: Nlinks=%i, size=%li, num=%i\n", inode->nlinks, inode->size, inode->num);
    //printf("Going to edit attributes\n");
    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_size = inode->size;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;
    //printf("edited attributes\n");
   // printf("S_IFDIR: %o\n", S_IFDIR);
    //printf("Mode %o\n", stbuf -> st_mode);


    return SUCCESS;
}



static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    printf("Entering wfs_mkdir\n");
    printf("wfs_mkdir: path = %s\n", path);
    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char childPath[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, childPath);
    int parentInodeIdx = find_inode(parentPath);
    printf("wfs_mkdir: Parent inode index: %i\n", parentInodeIdx);

    if (parentInodeIdx < 0) return -ENOENT;

    if (find_inode(childPath) >= 0) return -EEXIST;

    int childInodeIdx = allocate_inode();
    printf("wfs_mkdir: Inode index: %i\n", childInodeIdx);
    if (childInodeIdx < 0) return -ENOSPC;

    struct wfs_inode childInode = {0};
    childInode.mode = (mode) | S_IFREG;
    childInode.num = childInodeIdx;
    childInode.nlinks = 2;
    childInode.uid = getuid();
    childInode.gid = getgid();
    childInode.atim = childInode.mtim = childInode.ctim = time(NULL);
    childInode.size = 0;
    for (int i = 0; i < N_BLOCKS; i++) {
        childInode.blocks[i] = -1;
    }

    printf("wfs_mkdir: Starting directory entry logic!\n");
    struct wfs_inode* parentInode = get_inode(parentInodeIdx);

    // Find Block and dEntry
    int blockIdx = -1;
    int dataBlockIdx = -1;
    int dentryIdx = -1;
    int allocd = -1;
    // Find first empty block (allocate block and use first dentry) or first empty dentry within alloced block
    for (int i = 0; i < N_BLOCKS; i++) {
        if (parentInode->blocks[i] == -1) {
            printf("directory logic: first parent block empty allocing\n");
            // Allocate a new block for directory entries
            dataBlockIdx = allocate_data_block();
            if (dataBlockIdx < 0){
                printf("directory logic: BAD ALLOCATED BLOCK\n");
                return -ENOSPC;
            }
            blockIdx = i;
            dentryIdx = 0;
            allocd = 1;
            printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
            break;
        } else {
            struct wfs_dentry *dentry = (struct wfs_dentry*)(disks[0] + superblock->d_blocks_ptr + parentInode->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j * sizeof(struct wfs_dentry) < BLOCK_SIZE; j++) {
                if (dentry[j].num <= 0) {
                    printf("directory logic: found room in alloced dBlock\n");
                    blockIdx = i;
                    dataBlockIdx = parentInode->blocks[i];
                    dentryIdx = j;
                    printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
                    break;
                }
            }
            if (blockIdx != -1 && dentryIdx != -1) break;
        }
    }

    if (blockIdx == -1 && dentryIdx == -1) return -ENOSPC;

    // Create
    struct wfs_dentry entry = {0};
    strncpy(entry.name, childPath, MAX_NAME - 1);
    entry.num = childInodeIdx;

    // Put in
    printf("directory logic: updating disk info\n");
    for(int i = 0; i < superblock->num_disks; i++) {
        // Place childInode
        printf("directory logic: placing childInode into disk=%d childInodeIdx=%d\n", i, childInodeIdx);
        memcpy(disks[i] + superblock->i_blocks_ptr + childInodeIdx * BLOCK_SIZE, &childInode, BLOCK_SIZE);

        struct wfs_inode *parentInodePtr = (struct wfs_inode*)(disks[i] + superblock->i_blocks_ptr + parentInodeIdx * BLOCK_SIZE);
        // Update blocks if we alloced
        if (allocd == 1) {
            printf("directory logic: alloc -> updating alloc disk info\n");
            parentInodePtr->blocks[blockIdx] = dataBlockIdx;
            memset((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE), 0, BLOCK_SIZE);
            printf("directory logic: set parentInodePtr->blocks[%d]=%d\n", blockIdx, dataBlockIdx);
            printf("directory logic: cleared dataBlockIdx=%d\n", dataBlockIdx);
        } else {
            // Only clear entry
            printf("directory logic: no alloc -> clearing parentInode->blocks[%d]\n", blockIdx);
            memset(disks[i] + superblock->d_blocks_ptr + parentInode->blocks[blockIdx] * BLOCK_SIZE + dentryIdx * sizeof(struct wfs_dentry), 0, sizeof(struct wfs_dentry));
        }

        // Copy in dentry
        // printf("directory logic: copying in dentry \n", blockIdx);
        memcpy((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE + dentryIdx * sizeof(struct wfs_dentry)), &entry, sizeof(struct wfs_dentry));
        printf("directory logic: copied entry into dataBlockIdx=%d dentryIdx=%d\n", dataBlockIdx, dentryIdx);

        // Edit parentInode
        parentInodePtr->nlinks++;
        parentInodePtr->size += sizeof(struct wfs_dentry);
    }

    printf("Returning from mkdir\n");
    return SUCCESS;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    printf("Entering wfs_mkdir\n");
    printf("wfs_mkdir: path = %s\n", path);
    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char childPath[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, childPath);
    int parentInodeIdx = find_inode(parentPath);
    printf("wfs_mkdir: Parent inode index: %i\n", parentInodeIdx);

    if (parentInodeIdx < 0) return -ENOENT;

    if (find_inode(childPath) >= 0) return -EEXIST;

    int childInodeIdx = allocate_inode();
    printf("wfs_mkdir: Inode index: %i\n", childInodeIdx);
    if (childInodeIdx < 0) return -ENOSPC;

    struct wfs_inode childInode = {0};
    childInode.mode = (mode & 0777) | S_IFDIR;
    childInode.num = childInodeIdx;
    childInode.nlinks = 2;
    childInode.uid = getuid();
    childInode.gid = getgid();
    childInode.atim = childInode.mtim = childInode.ctim = time(NULL);
    childInode.size = 0;
    for (int i = 0; i < N_BLOCKS; i++) {
        childInode.blocks[i] = -1;
    }

    printf("wfs_mkdir: Starting directory entry logic!\n");
    struct wfs_inode* parentInode = get_inode(parentInodeIdx);

    // Find Block and dEntry
    int blockIdx = -1;
    int dataBlockIdx = -1;
    int dentryIdx = -1;
    int allocd = -1;
    // Find first empty block (allocate block and use first dentry) or first empty dentry within alloced block
    for (int i = 0; i < N_BLOCKS; i++) {
        if (parentInode->blocks[i] == -1) {
            printf("directory logic: first parent block empty allocing\n");
            // Allocate a new block for directory entries
            dataBlockIdx = allocate_data_block();
            if (dataBlockIdx < 0){
                printf("directory logic: BAD ALLOCATED BLOCK\n");
                return -ENOSPC;
            }
            blockIdx = i;
            dentryIdx = 0;
            allocd = 1;
            printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
            break;
        } else {
            struct wfs_dentry *dentry = (struct wfs_dentry*)(disks[0] + superblock->d_blocks_ptr + parentInode->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j * sizeof(struct wfs_dentry) < BLOCK_SIZE; j++) {
                if (dentry[j].num <= 0) {
                    printf("directory logic: found room in alloced dBlock\n");
                    blockIdx = i;
                    dataBlockIdx = parentInode->blocks[i];
                    dentryIdx = j;
                    printf("directory logic: location - parentInode->blocks[%d]=%d at dentryIdx=%d\n", blockIdx, dataBlockIdx, dentryIdx);
                    break;
                }
            }
            if (blockIdx != -1 && dentryIdx != -1) break;
        }
    }

    if (blockIdx == -1 && dentryIdx == -1) return -ENOSPC;

    // Create
    struct wfs_dentry entry = {0};
    strncpy(entry.name, childPath, MAX_NAME - 1);
    entry.num = childInodeIdx;

    // Put in
    printf("directory logic: updating disk info\n");
    for(int i = 0; i < superblock->num_disks; i++) {
        // Place childInode
        printf("directory logic: placing childInode into disk=%d childInodeIdx=%d\n", i, childInodeIdx);
        memcpy(disks[i] + superblock->i_blocks_ptr + childInodeIdx * BLOCK_SIZE, &childInode, BLOCK_SIZE);

        struct wfs_inode *parentInodePtr = (struct wfs_inode*)(disks[i] + superblock->i_blocks_ptr + parentInodeIdx * BLOCK_SIZE);
        // Update blocks if we alloced
        if (allocd == 1) {
            printf("directory logic: alloc -> updating alloc disk info\n");
            parentInodePtr->blocks[blockIdx] = dataBlockIdx;
            memset((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE), 0, BLOCK_SIZE);
            printf("directory logic: set parentInodePtr->blocks[%d]=%d\n", blockIdx, dataBlockIdx);
            printf("directory logic: cleared dataBlockIdx=%d\n", dataBlockIdx);
        } else {
            // Only clear entry
            printf("directory logic: no alloc -> clearing parentInode->blocks[%d]\n", blockIdx);
            memset(disks[i] + superblock->d_blocks_ptr + parentInode->blocks[blockIdx] * BLOCK_SIZE + dentryIdx * sizeof(struct wfs_dentry), 0, sizeof(struct wfs_dentry));
        }

        // Copy in dentry
        // printf("directory logic: copying in dentry \n", blockIdx);
        memcpy((disks[i] + superblock->d_blocks_ptr + dataBlockIdx * BLOCK_SIZE + dentryIdx * sizeof(struct wfs_dentry)), &entry, sizeof(struct wfs_dentry));
        printf("directory logic: copied entry into dataBlockIdx=%d dentryIdx=%d\n", dataBlockIdx, dentryIdx);

        // Edit parentInode
        parentInodePtr->nlinks++;
        parentInodePtr->size += sizeof(struct wfs_dentry);
    }

    printf("Returning from mkdir\n");
    return SUCCESS;
}

static int wfs_unlink(const char *path) {
    printf("Entering wfs_unlink\n"); 
    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        fprintf(stderr, "Found invalid inode index\n");
        return FAIL;
    }
    struct wfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "Invalid inode index, cannot find inode\n");
        return FAIL;
    }

    inode->nlinks--;
    if (inode->nlinks == 0) {
        char *bitmap = (char *)disks[0] + superblock->i_bitmap_ptr;
        bitmap[inode_idx / 8] &= ~(1 << (inode_idx % 8));
    }
    return SUCCESS;
}

static int wfs_rmdir(const char *path) {
    printf("Entering wfs_rmdir\n"); 
    return wfs_unlink(path);
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("Entering wfs_read\n");
    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        fprintf(stderr, "Found invalid inode index\n");
        return -ENOENT;
    }
    struct wfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "Invalid inode index, cannot find inode\n");
        return -ENOENT;
    }

    if (offset >= inode->size) return FAIL;
    if (offset + size > inode->size) size = inode->size - offset;

    memcpy(buf, (char *)get_block(inode->blocks[0]) + offset, size);
    return size;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("Entering wfs_write\n"); 
    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        fprintf(stderr, "Found invalid inode index\n");
        return -ENOENT;
    }
    struct wfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        fprintf(stderr, "Invalid inode index, cannot find inode\n");
        return -ENOENT;
    }

    memcpy((char *)get_block(inode->blocks[0]) + offset, buf, size);
    inode->size = offset + size;
    return size;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    printf("Entering wfs_readdir, path is: %s\n", path);
    
    // Add the standard directory entries
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    int inode_idx = find_inode(path);
    if (inode_idx < 0) {
        return -ENOENT;
    }
    
    struct wfs_inode *inode = get_inode(inode_idx);
    if (!inode) {
        return -ENOENT;
    }

    if (!(inode->mode & S_IFDIR)) {
        return -ENOTDIR;
    }

    // Print debug information
    printf("Directory size: %ld\n", inode->size);
    printf("First block: %ld\n", inode->blocks[0]);

    // Directly read entries from the first block
    size_t num_entries = inode->size / sizeof(struct wfs_dentry);
    // This number is too large because the size was incremented multiple times
    struct wfs_dentry *entries = (struct wfs_dentry*)(disks[0] + 
            superblock->d_blocks_ptr + inode->blocks[0] * BLOCK_SIZE);
        
    for (size_t i = 0; i < num_entries; i++) {
        printf("entries for %li- %i\n", i, entries[i].num);
        if (entries[i].num != 0) {  // Valid entry
            printf("Found entry: %s (inode: %d)\n", entries[i].name, entries[i].num);
            if (filler(buf, entries[i].name, NULL, 0)) {
                printf("filler buf full\n");
                break;
            }
        }
        printf("readdir iteration: %li\n", i);
    }

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
        return FAIL;
    }

    disks = malloc(sizeof(void *) * num_disks);
    if (disks == NULL) {
        fprintf(stderr, "Memory allocation failed for disks\n");
        return FAIL;
    }

    fileDescs = malloc(sizeof(int) * num_disks);
    if (fileDescs == NULL) {
        fprintf(stderr, "Memory allocation failed for fileDescs\n");
        return FAIL;
    }

    for (int i = 0; i < num_disks; i++) {
        fileDescs[i] = open(argv[i + 1], O_RDWR);
        if (fileDescs[i] == -1) {
            fprintf(stderr, "Failed to open disk %s\n", argv[i + 1]);
            return FAIL;
        }

        struct stat st;
        if (fstat(fileDescs[i], &st) != 0) {
            fprintf(stderr, "Failed to get disk size for %s\n", argv[i + 1]);
            return FAIL;
        }
        diskSize = st.st_size;

        disks[i] = mmap(NULL, diskSize, PROT_READ | PROT_WRITE, MAP_SHARED, fileDescs[i], 0);
        if (disks[i] == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap disk %s\n", argv[i + 1]);
            return FAIL;
        }
    }

    superblock = (struct wfs_sb *)disks[0];

    if (superblock == NULL) {
        fprintf(stderr, "Failed to access superblock\n");
        return FAIL;
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
            return FAIL;
        }
        close(fileDescs[i]);
    }

    free(disks);
    free(fileDescs);

    return rc;
}
