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
struct allocInts {
  int returnInt;
  int isUsed;
};

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

struct allocInts allocate_data_block(struct wfs_inode* parentInode) {
    struct allocInts returnValue = { -ENOSPC, -ENOSPC };
    printf("Entering allocate_data_block\n");
    printf("Parent Inode in Alloc Data Block: %d\n", parentInode->num);
    printf("Num of blocks: %zd\n", superblock->num_data_blocks);

    // Iterate through parent inode's blocks
    for (int i = 0; i < N_BLOCKS; i++) {
        // If this block is unused in the inode
        if (parentInode->blocks[i] == -1) {
            // Allocate a new block
            for (int blockNum = 0; blockNum < superblock->num_data_blocks; blockNum++) {
                int is_free = 1;

                // Check block availability across all disks
                for (int disk = 0; disk < superblock->num_disks; disk++) {
                    char *data_bitmap = (char*)disks[disk] + superblock->d_bitmap_ptr;
                    
                    // Check if block is already used
                    if (data_bitmap[blockNum / 8] & (1 << (blockNum % 8))) {
                        is_free = 0;
                        break;
                    }
                }

                // If block is free, mark it as used and return
                if (is_free) {
                    for (int disk = 0; disk < superblock->num_disks; disk++) {
                        char *data_bitmap = (char*)disks[disk] + superblock->d_bitmap_ptr;
                        // Mark block as used on bitmap
                        data_bitmap[blockNum / 8] |= (1 << (blockNum % 8));
                        
                        // Zero out the block
                        memset(disks[disk] + superblock->d_blocks_ptr + blockNum * BLOCK_SIZE, 0, BLOCK_SIZE);
                        
                        printf("Marked block %d as used on disk %d\n", blockNum, disk);
                    }

                    returnValue.returnInt = blockNum;
                    returnValue.isUsed = 1;
                    return returnValue;
                }
            }
        }
        // If block is already allocated, check its dentries
        else {
            char *blockAddr = (char*)disks[0] + superblock->d_blocks_ptr + parentInode->blocks[i] * BLOCK_SIZE;
            struct wfs_dentry *entries = (struct wfs_dentry*)blockAddr;
            
            // Check each dentry in the block
            for (int k = 0; k * sizeof(struct wfs_dentry) < BLOCK_SIZE; k++) {
                printf("Dentry number: %d\n", k);
                printf("Dentry->num: %d\n", entries[k].num);
                
                // If dentry is empty, we can use this block
                if (entries[k].num <= 0) {
                    printf("Empty dentry found\n");
                    returnValue.returnInt = parentInode->blocks[i];
                    returnValue.isUsed = 1;
                    return returnValue;
                }
            }
        }
    }

    printf("No free blocks found\n");
    return returnValue;  // No space left
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

static int add_parent_dir_entry(off_t parentIdx, const char *name, off_t newIdx) {
    printf("Entering parent dentry adding\n");
    printf("Path: %s\n", name);
    printf("Parent ID: %zd\n", parentIdx);
    printf("New ID: %zd\n", newIdx);

    printf("how many dentries in one block: %li\n", BLOCK_SIZE/sizeof(struct wfs_dentry));
    struct wfs_inode *parentInode = get_inode(parentIdx);
    
    // Calculate how many entries are currently in the directory
    size_t current_entries = parentInode->size / sizeof(struct wfs_dentry);
    size_t entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);
    printf("Current entries in parent: %zd\n", current_entries);
    printf("Entries per block: %zd\n", entries_per_block);
    
    // Calculate which block we need and the offset within that block
    size_t block_idx = current_entries / entries_per_block;
    size_t entry_offset = current_entries % entries_per_block;
    printf("Which block needed: %zd\n", block_idx);
    printf("Entry offset: %zd\n", entry_offset);
    
    // Check if we need a new block
    if (block_idx >= N_BLOCKS) {
        printf("No new blocks available\n");
        return -ENOSPC;  // No more blocks available
    }
    
    // Allocate new block if needed
    bool is_new_block = false;
    bool is_used_bool = false;
    printf("Need new block\n");
    if (parentInode->blocks[block_idx] == 0) {
        struct allocInts newBlock = allocate_data_block(parentInode);
        if (newBlock.returnInt < 0) {
            printf("New block not allocated\n");
            return newBlock.returnInt;
        }
        parentInode->blocks[block_idx] = newBlock.returnInt;
        is_used_bool = true;
        is_new_block = true;
        printf("New block found\n");
    }
    
    // Only zero out the block if it's newly allocated
    if (is_new_block && !is_used_bool) {
        for (int disk = 0; disk < superblock->num_disks; disk++) {
          printf("Zeroing out block in disk %d\n", disk);
          char *blockAddr = (char*)disks[disk] + superblock->d_blocks_ptr + 
                          parentInode->blocks[block_idx] * BLOCK_SIZE + entry_offset;
          printf("Block number: %zd\n", block_idx);
          memset(blockAddr, 0, BLOCK_SIZE);
        }
    }
    
    // Create the new directory entry
    printf("Creating new directory entry\n");
    struct wfs_dentry newEntry;
    strncpy(newEntry.name, name, MAX_NAME - 1);
    newEntry.name[MAX_NAME - 1] = '\0';
    newEntry.num = newIdx;
    
    // Write the new entry to all disks
    for (int disk = 0; disk < superblock->num_disks; disk++) {
      printf("Disk: %d\n", disk);
      // Calculate entry position in this disk
      char *blockAddr = (char*)disks[disk] + superblock->d_blocks_ptr + 
                        parentInode->blocks[block_idx] * BLOCK_SIZE;
      printf("Block Address: block pointer: %zd, block in node: %zd\n", superblock->d_blocks_ptr, parentInode->blocks[block_idx]);
      struct wfs_dentry *entries = (struct wfs_dentry*)blockAddr;
      
      // Write the new entry at the correct offset without disturbing existing entries
      memcpy(&entries[entry_offset], &newEntry, sizeof(struct wfs_dentry));
      printf("Mem-copied\n");
    }
    
    // Update parent inode
    printf("Updating parent Inode\n");
    parentInode->size += sizeof(struct wfs_dentry);
    parentInode->nlinks++;
    
    // Write updated inode to all disks
    for (int disk = 0; disk < superblock->num_disks; disk++) {
      printf("Writing updated inode to disk %d\n", disk);
      struct wfs_inode *diskInode = (struct wfs_inode*)
          ((char*)disks[disk] + superblock->i_blocks_ptr + parentIdx * BLOCK_SIZE);
      memcpy(diskInode, parentInode, sizeof(struct wfs_inode));
      msync(disks[disk], diskSize, MS_SYNC);
    }
    
    printf("Finished adding dentry to parent\n");
    return 0;
}

static void write_inode(off_t idx, struct wfs_inode* newInode){
    printf("Entering write_inode\n");
    struct wfs_inode *inodeAddr = (struct wfs_inode*) get_inode_block_addr(0, idx);
    memcpy(inodeAddr, newInode, BLOCK_SIZE);
    printf("Exiting write_inode\n");
    return;
}

static void write_inode_across_disks(off_t idx, struct wfs_inode* newInode) {
    printf("Entering write_inode_across_disks\n");
    
    // Write the inode to the inode block on ALL disks
    for (int disk = 0; disk < superblock->num_disks; disk++) {
        // Calculate the inode address for this specific disk
        struct wfs_inode *inodeAddr = (struct wfs_inode*)
            ((char*)disks[disk] + superblock->i_blocks_ptr + idx * BLOCK_SIZE);
        
        // Copy the entire inode block
        memcpy(inodeAddr, newInode, BLOCK_SIZE);
    }
    
    printf("Exiting write_inode_across_disks\n");
}

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
    printf("Entering wfs_mknod\n");
    printf("Inode mode: %o\n", mode);
    char parentPath[MAX_PATH_NAME];
    char origPath[MAX_PATH_NAME];
    char childPath[MAX_PATH_NAME];
    strcpy(origPath, path);
    split_path(origPath, parentPath, childPath);
    int parentInode = find_inode(parentPath);

    if (parentInode < 0)
        return -ENOENT;

    if (find_inode(childPath) >= 0)
        return -EEXIST;

    int child_idx = allocate_inode();
    if (child_idx < 0)
        return -ENOSPC;

    struct wfs_inode inode = {0};
    inode.num = child_idx;
    inode.mode = mode | S_IFREG; //| //__S_IFREG;
    inode.nlinks = 1;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.atim = inode.mtim = inode.ctim = time(NULL);
    inode.size = 0;
    // int firstBlock = allocate_data_block(parentInode);
    // if (firstBlock >= 0) {
    //     memset((disks[0] + superblock->d_blocks_ptr + firstBlock * BLOCK_SIZE), 0, BLOCK_SIZE);
    //     inode.blocks[0] = firstBlock;
    // }
    // else
    // {
    //     printf("wfs_mknod: BAD FIRST BLOCK\n");
    // }
    write_inode(child_idx, &inode);

    add_parent_dir_entry(parentInode, childPath, child_idx);
    printf("Returning from mknod\n");
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
    int parentInode = find_inode(parentPath);
    printf("Parent inode index: %i\n", parentInode);

    if (parentInode < 0) return -ENOENT;


    if (find_inode(childPath) >= 0)
        return -EEXIST;

    int inodeIndex = allocate_inode();
    printf("Inode index: %i\n", inodeIndex);
    if (inodeIndex < 0){
        return -ENOSPC;
    }
    

    struct wfs_inode inode = {0};
    inode.mode = (mode & 0777) | S_IFDIR; //| //__S_IFREG;
    inode.num = inodeIndex;
    inode.nlinks = 2;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.atim = inode.mtim = inode.ctim = time(NULL);
    inode.size = 0;
    //inode.blocks[0] = firstBlock;

    //struct wfs_inode* p = get_inode(parentInode);



    // char zeroBlock[BLOCK_SIZE] = {0};
    // for (int i = 0; i < superblock->num_disks; i++) {
    //     memcpy((char*)disks[i] + superblock->d_blocks_ptr + firstBlock * BLOCK_SIZE, 
    //            zeroBlock, BLOCK_SIZE);
    // }

    // Write inode across all disks
    write_inode_across_disks(inodeIndex, &inode);

    // for (int i = 0; i < superblock->num_disks; i++) {
    //     memcpy((char*)disks[i] + superblock->i_blocks_ptr + inodeIndex * BLOCK_SIZE,
    //            &inode, sizeof(struct wfs_inode));
    // }

    // Add the new directory to its parent's directory entries
    int result = add_parent_dir_entry(parentInode, childPath, inodeIndex);
    if (result < 0) {
        // Cleanup if adding to parent fails
        return result;
    }

    // if(p.blocks[0] == 0 && p.size == 0){
    //     int blockIndex = allocate_data_block();
    //     printf("Allocated block: %i\n", blockIndex);
    //     if(blockIndex < 0){
    //         return -ENOSPC;
    //     }

    //     p.blocks[0] = blockIndex;

    //     char zBlock[BLOCK_SIZE] = {0};
    //     for(int i = 0; i < superblock -> num_disks; i++){
    //         memcpy((char*) disks[i] + superblock->d_blocks_ptr + blockIndex * BLOCK_SIZE, zBlock, BLOCK_SIZE);
    //     }
    // }

    /*struct wfs_dentry entry = {0};
    strncpy(entry.name, childPath, MAX_NAME-1);
    entry.num = inodeIndex;

    int found_block = -1;
    for (int i = 0; i < N_BLOCKS; i++) {
        if (p->blocks[i] == 0 && p->size == 0) {
            // Allocate a new block for the parent's directory entries if needed
            int blockIndex = allocate_data_block();
            if (blockIndex < 0) {
                return -ENOSPC;
            }
            //p->blocks[i] = blockIndex;
            //found_block = i;
            //break;
            for (int disk = 0; disk < superblock->num_disks; disk++) {
                struct wfs_inode *diskParentInodePtr = (struct wfs_inode*)((char*)disks[disk] + superblock->i_blocks_ptr + parentInode * BLOCK_SIZE);
                diskParentInodePtr->blocks[i] = blockIndex;
            }
            found_block = i;
        
            // Zero out the block on ALL disks
            char zeroBlock[BLOCK_SIZE] = {0};
            for (int disk = 0; disk < superblock->num_disks; disk++) {
                memcpy((char*)disks[disk] + superblock->d_blocks_ptr + blockIndex * BLOCK_SIZE, 
                       zeroBlock, BLOCK_SIZE);
            }
            
            break;
        }
    }

    if (found_block >= 0) {
        // Add directory entry to ALL disks identically
        for (int disk = 0; disk < superblock->num_disks; disk++) {
            struct wfs_dentry *entries = (struct wfs_dentry*)(
                disks[disk] + superblock->d_blocks_ptr + 
                p->blocks[found_block] * BLOCK_SIZE
            );

            // Find first empty entry
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (entries[j].num == 0) {
                    memcpy(&entries[j], &entry, sizeof(struct wfs_dentry));
                    break;
                }
            }
        }

        // Update parent inode size and nlinks on ALL disks
        for (int disk = 0; disk < superblock->num_disks; disk++) {
            struct wfs_inode *diskParentInodePtr = (struct wfs_inode*)
                ((char*)disks[disk] + superblock->i_blocks_ptr + parentInode * BLOCK_SIZE);
            diskParentInodePtr->size += sizeof(struct wfs_dentry);
            diskParentInodePtr->nlinks++;
        }
    }


    // p.size += sizeof(struct wfs_dentry);
    // p.nlinks++;

    for(int i = 0; i < superblock -> num_disks; i++){
        memcpy((char*) disks[i] + superblock->i_blocks_ptr + inodeIndex * BLOCK_SIZE,
            &inode, sizeof(struct wfs_inode));

        memcpy((char*) disks[i] + superblock -> i_blocks_ptr + parentInode * BLOCK_SIZE,
            p, sizeof(struct wfs_inode));

        // memcpy((char*) disks[i] + superblock -> d_blocks_ptr + p.blocks[0] * BLOCK_SIZE + p.size - sizeof(struct wfs_dentry),
        //     &entry, sizeof(struct wfs_dentry));

        // msync(disks[i], superblock -> i_blocks_ptr + (inodeIndex + 1) * BLOCK_SIZE, MS_SYNC);
        // msync(disks[i], superblock -> d_blocks_ptr + (p.blocks[0] + 1) * BLOCK_SIZE, MS_SYNC);
    }
    //write_inode(inodeIndex, &inode);

    //add_dir_entry(parentInode, newPath, inodeIndex);
    printf("Returning from mkdir\n");
    return SUCCESS;*/
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
