
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

            struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[0] + inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
                if (strcmp(dir_entries[j].name, token) == 0) {
		   

                    inode = (struct wfs_inode *)(inode_table + (dir_entries[j].num * BLOCK_SIZE));
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

void check_inode() {
    char *inode_bitmap = (char *)disks[0] + superblock->i_bitmap_ptr;
    for (int i = 0; i < superblock->num_inodes; i++) {
        printf("inode at %d: %d\n", i, (inode_bitmap[i / 8] & (0x1 << (i % 8))));
    }
}


int allocate_inode(char *disk) {
    if (superblock->raid_mode == 0) { // RAID 0 Mode
        // Iterate through all possible inodes to find a free one
        for (int i = 0; i < superblock->num_inodes; i++) {
            int byte = i / 8; // Determine the byte in the bitmap
            int bit = i % 8;  // Determine the bit in the byte
            int is_free = 1;  // Assume the inode is free

            // Check all disks for the same inode bit
            for (int d = 0; d < diskNum; d++) {
                char *inode_bitmap = (char *)disks[d] + superblock->i_bitmap_ptr;
                if (inode_bitmap[byte] & (1 << bit)) { // If allocated on any disk
                    is_free = 0; // Mark as not free
                    break;
                }
            }

            if (is_free) { // If the inode is free on all disks
                // Allocate the inode on all disks
                for (int d = 0; d < diskNum; d++) {
                    char *inode_bitmap = (char *)disks[d] + superblock->i_bitmap_ptr;
                    inode_bitmap[byte] |= (1 << bit); // Mark allocated
                }
                return i; // Return the inode number
            }
        }
    } else {
        // Standard (non-RAID 0) Mode
        char *inode_bitmap = disk + superblock->i_bitmap_ptr;
        for (int i = 0; i < superblock->num_inodes; i++) {
            if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) { // Free inode
                inode_bitmap[i / 8] |= (1 << (i % 8));    // Mark allocated
                return i;
            }
        }
    }

    return -ENOSPC; // No free inodes
}


//int allocate_inode(char *disk) {
//	// synchronize across disks if raid 0
//    if(sb->raid_mode == 0) {
//	    for(int i = 0; i < num_disks; i++){
//    		char *inode_bitmap = (char *)disk_maps[i] + sb->i_bitmap_ptr;
//    		for (int i = 0; i < sb->num_inodes; i++) { 
//    		    if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) { // Free inode
//    		        inode_bitmap[i / 8] |= (1 << (i % 8));    // Mark allocated
//    		        return i;
//    		    }
//    		}
//	    }
//    } else {
//    	char *inode_bitmap = disk + sb->i_bitmap_ptr;
//    	for (int i = 0; i < sb->num_inodes; i++) {
//    	    if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) { // Free inode
//    	        inode_bitmap[i / 8] |= (1 << (i % 8));    // Mark allocated
//    	        return i;
//    	    }
//    	}
//    }
//    return -ENOSPC;  // No free inodes
//}

int allocate_block(char *disk) {
    printf("RAID MODE: %d\n", superblock->raid_mode);
    if (superblock->raid_mode == 0) { // RAID 0 Mode
        // RAID0: Striping across multiple disks
	printf("RAID 0\n");
        for (int i = 0; i < superblock->num_data_blocks; i++) {
            int disk_index = i % diskNum;                 // Determine the disk for this block
            int logical_block_num = i / diskNum;          // Block number on the selected disk
            int byte = logical_block_num / 8;               // Byte in the bitmap
            int bit = logical_block_num % 8;                // Bit in the byte
	    printf("DISK INDEX: %d\n", disk_index);

            // Pointer to the data bitmap of the selected disk
            uint8_t *current_data_bitmap = (uint8_t *)disks[disk_index] + superblock->d_bitmap_ptr;

            if (!(current_data_bitmap[byte] & (1 << bit))) { // Free block on selected disk
                current_data_bitmap[byte] |= (1 << bit);     // Mark the block as allocated

                // Zeroing out the newly allocated block
                char zero_block[BLOCK_SIZE];
                memset(zero_block, 0, BLOCK_SIZE);
                char *block_ptr = (char *)disks[disk_index] + superblock->d_blocks_ptr + (logical_block_num * BLOCK_SIZE);
                memcpy(block_ptr, zero_block, BLOCK_SIZE);

                printf("Allocating data block number: %d on disk %d\n", logical_block_num, disk_index);

                return i; // Return the logical block number
            }
        }
    } else {
        // RAID 1 or no RAID: Allocate using standard logic
        char *data_bitmap = disk + superblock->d_bitmap_ptr;
        for (int i = 0; i < superblock->num_data_blocks; i++) {
            if (!(data_bitmap[i / 8] & (1 << (i % 8)))) { // Free block
                data_bitmap[i / 8] |= (1 << (i % 8));     // Mark allocated
                printf("Allocating data block number: %d\n", i);
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
        return -ENOENT;  // File or directory not found
    }

    stbuf->st_mode = inode->mode;    // File mode
    stbuf->st_nlink = inode->nlinks; // Number of links
    stbuf->st_uid = inode->uid;      // User ID
    stbuf->st_gid = inode->gid;      // Group ID
    stbuf->st_size = inode->size;    // File size
    stbuf->st_atime = inode->atim;   // Last access time
    stbuf->st_mtime = inode->mtim;   // Last modification time
    stbuf->st_ctime = inode->ctim;   // Last status change time

    return SUCCEED;
}


// FUSE: mkdir implementation
static int wfs_mkdir_helper(const char *path, mode_t mode, char *disk) {
    
    printf("mkdir callback\n");	
    char parent_path[1024], new_name[MAX_NAME];
    strncpy(parent_path, path, sizeof(parent_path));
    char *slash = strrchr(parent_path, '/');
    if (!slash || slash == parent_path) {
        strncpy(new_name, path + 1, sizeof(new_name)); // Root directory
        strcpy(parent_path, "/");
    } else {
        strncpy(new_name, slash + 1, sizeof(new_name));
        *slash = '\0';
    }

    struct wfs_inode *parent_inode = get_inode(parent_path, disk);
    printf("parent path: %s", parent_path);
    if (!parent_inode) {
        return -ENOENT;  // Parent directory does not exist
    }

    // Allocate a new inode for the directory
    int new_inode_index = allocate_inode(disk);
    if (new_inode_index < 0) {
        return new_inode_index;  // Propagate ENOSPC
    }
    printf("new inode index: %d\n", new_inode_index);

    if(superblock->raid_mode == 0) {
   	for(int i = 0; i < diskNum; i++) {
		char *inode_table = ((char *)disks[i] + superblock->i_blocks_ptr);
	        struct wfs_inode *new_inode =(struct wfs_inode *) (inode_table + (new_inode_index * BLOCK_SIZE));
	        printf("allocating inode at index: %d, disk: %d\n", new_inode_index, i);
	
	        memset(new_inode, 0, BLOCK_SIZE);
	        new_inode->num = new_inode_index;
	        new_inode->mode = S_IFDIR | mode; // Set directory mode
	        new_inode->nlinks = 2;           // "." and parent's link
	        new_inode->size = 0;             // Empty directory initially
	        new_inode->atim = new_inode->mtim = new_inode->ctim = time(NULL);
	        new_inode->uid = getuid();
	        new_inode->gid = getgid();

	}	
    } else {
    	char *inode_table = (disk + superblock->i_blocks_ptr);
    	struct wfs_inode *new_inode =(struct wfs_inode *) (inode_table + (new_inode_index * BLOCK_SIZE));
    	printf("allocating inode at index raid 1: %d\n", new_inode_index);

    	memset(new_inode, 0, BLOCK_SIZE);
    	new_inode->num = new_inode_index;
    	new_inode->mode = S_IFDIR | mode; // Set directory mode
    	new_inode->nlinks = 2;           // "." and parent's link
    	new_inode->size = 0;             // Empty directory initially
    	new_inode->atim = new_inode->mtim = new_inode->ctim = time(NULL);
    	new_inode->uid = getuid();
    	new_inode->gid = getgid();
    }
//    new_inode->blocks[0] = sb->d_blocks_ptr + block_index * BLOCK_SIZE;

    // Add new directory entry to the parent
    //int block_index = -1;
    int found_space = 0;
    printf("parent inode num: %d\n", parent_inode->num);
    // Add new directory entry to the parent
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] == 0) {
            // Allocate a new block for the parent directory
	    printf("CALLING ALLOCATE BLOCK\n");
            int block_index = allocate_block(disk);
    	    if (block_index < 0) {
        		return block_index;  // Propagate ENOSPC
    		}
	    printf("allocating data block\n");
	    if(superblock->raid_mode == 0) {
		for(int d = 0; d < diskNum; d++) {
		    //int disk_index = block_index % num_disks;
		    int logical_block_num = block_index / diskNum; 
	    	    struct wfs_inode *sync_inode = get_inode(parent_path, (char *)disks[d]);
		    sync_inode->blocks[i] = superblock->d_blocks_ptr + logical_block_num * BLOCK_SIZE;

		}
	    } else {
	    
	    	parent_inode->blocks[i] = superblock->d_blocks_ptr + block_index * BLOCK_SIZE;
	    }
        }

        struct wfs_dentry *dir_entries = (struct wfs_dentry *)(disk + parent_inode->blocks[i]);
        // Try to find an empty entry in the current block
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dir_entries[j].num == 0) { // Empty entry
                strncpy(dir_entries[j].name, new_name, MAX_NAME);
                dir_entries[j].num = new_inode_index;
                parent_inode->size += sizeof(struct wfs_dentry);
                found_space = 1;
                break;
            }
        }

        // If space is found in the current block, stop checking further blocks
        if (found_space) {
            break;
        }
    }

    // If no space was found, return ENOSPC
    if (!found_space) {
        return -ENOSPC;
    }

    // Finally, assign the block for the new directory's contents (blocks[0])
    //new_inode->blocks[0] = sb->d_blocks_ptr + block_index * BLOCK_SIZE;

    return SUCCEED;  // SUCCEED
}

static int wfs_mkdir(const char *path, mode_t mode) {
	if(superblock->raid_mode == 0) {
		wfs_mkdir_helper(path, mode, (char *)disks[0]);
	} else {
	for(int i = 0; i < diskNum; i++) {
		wfs_mkdir_helper(path, mode, (char *)disks[i]);
	}	
	}
	return SUCCEED;
}

static int wfs_mknod_helper(const char *path, mode_t mode, char *disk) {
    char parent_path[1024], new_name[MAX_NAME];
    strncpy(parent_path, path, sizeof(parent_path));
    char *slash = strrchr(parent_path, '/');
    if (!slash || slash == parent_path) {
        strncpy(new_name, path + 1, sizeof(new_name));
        strcpy(parent_path, "/");
    } else {
        strncpy(new_name, slash + 1, sizeof(new_name));
        *slash = '\0';
    }

    struct wfs_inode *parent_inode = get_inode(parent_path, disk);
    if (!parent_inode) {
        return -ENOENT;  // Parent directory not found
    }

    int new_inode_index = allocate_inode(disk);
    if (new_inode_index < 0) {
        return -ENOSPC;  // No free inodes
    }

    if(superblock->raid_mode == 0) {
    	for(int i = 0; i < diskNum; i++) {
                char *inode_table = ((char *)disks[i] + superblock->i_blocks_ptr);
                struct wfs_inode *new_inode =(struct wfs_inode *) (inode_table + (new_inode_index * BLOCK_SIZE));
                printf("allocating inode at index: %d, disk: %d\n", new_inode_index, i);

                memset(new_inode, 0, BLOCK_SIZE);
                new_inode->num = new_inode_index;
                new_inode->mode = S_IFREG | mode; // Set directory mode
                new_inode->nlinks = 1;           // "." and parent's link
                new_inode->size = 0;             // Empty directory initially
                new_inode->atim = new_inode->mtim = new_inode->ctim = time(NULL);
                new_inode->uid = getuid();
                new_inode->gid = getgid();

        }

    } else {
    	char *inode_table = disk + superblock->i_blocks_ptr;
    	struct wfs_inode *new_inode = (struct wfs_inode *)(inode_table + (new_inode_index * BLOCK_SIZE));
    	memset(new_inode, 0, BLOCK_SIZE);
    	new_inode->num = new_inode_index;
    	new_inode->mode = S_IFREG | mode;
    	new_inode->nlinks = 1;
    	new_inode->size = 0;
    	new_inode->atim = new_inode->mtim = new_inode->ctim = time(NULL);
    	new_inode->uid = getuid();
    	new_inode->gid = getgid();

    }
    // Add directory entry in parent
    int found_space = 0;
    for (int i = 0; i < D_BLOCK; i++) {
        if (parent_inode->blocks[i] == 0) {
            int block_index = allocate_block(disk);
            if (block_index < 0) {
                return -ENOSPC;  // No free blocks
            }
            parent_inode->blocks[i] = superblock->d_blocks_ptr + block_index * BLOCK_SIZE;
        }
        struct wfs_dentry *dir_entries = (struct wfs_dentry *)(disk + parent_inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dir_entries[j].num == 0) {
                strncpy(dir_entries[j].name, new_name, MAX_NAME);
                dir_entries[j].num = new_inode_index;
                parent_inode->size += sizeof(struct wfs_dentry);
                found_space = 1;
                break;
            }
        }
        if (found_space) {
            break;
        }
    }

    if (!found_space) {
        return -ENOSPC;  // Parent directory full
    }

    return SUCCEED;  // SUCCEED
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {

    int result = 0;
    if(superblock->raid_mode == 0) {
    	result = wfs_mknod_helper(path, mode, (char *)disks[0]);
    } else {
    for (int i = 0; i < diskNum; i++) {
        result = wfs_mknod_helper(path, mode, (char *)disks[i]);
        if (result < 0) {
            // Rollback any partial allocations
            for (int j = 0; j <= i; j++) {
                char *disk = (char *)disks[j];
                char *inode_bitmap = disk + superblock->i_bitmap_ptr;
                int inode_num = allocate_inode(disk);
                if (inode_num >= 0) {
                    inode_bitmap[inode_num / 8] &= ~(1 << (inode_num % 8));  // Free the inode
                }
            }
            return result;  // Propagate error
        }
    }
    }
    return result;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("readdir called for path: %s\n", path);

    // Find the inode for the directory
    struct wfs_inode *inode = get_inode(path, (char *)disks[0]);
    if (!inode) {
        return -ENOENT; // Directory not found
    }

    // Ensure the inode is a directory
    if (!S_ISDIR(inode->mode)) {
        return -ENOTDIR; // Not a directory
    }

    // Add "." and ".." entries
    filler(buf, ".", NULL, 0); // Current directory
    filler(buf, "..", NULL, 0); // Parent directory

    // Iterate through the blocks of the directory inode
    for (int i = 0; i < D_BLOCK; i++) {
        if (inode->blocks[i] == 0) break; // No more blocks

        // Access directory entries stored in the block
        struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[0] + inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (dir_entries[j].num != 0) { // Valid entry
                filler(buf, dir_entries[j].name, NULL, 0); // Add entry to the buffer
                printf("Adding entry: %s\n", dir_entries[j].name);
            }
        }
    }

    return SUCCEED; // SUCCEED
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Find the inode for the file
    struct wfs_inode *inode = get_inode(path, (char *)disks[0]);
    if (!inode) {
        return -ENOENT; // File not found
    }

    if (!S_ISREG(inode->mode)) {
        return -EISDIR; // Cannot read a directory
    }

    size_t bytes_read = 0;               // Track bytes read
    size_t block_offset = offset / BLOCK_SIZE;  // Determine starting block
    size_t block_start_offset = offset % BLOCK_SIZE; // Offset within the block

    while (bytes_read < size && block_offset < D_BLOCK + (BLOCK_SIZE / sizeof(uint32_t))) {
        void *data_block = NULL;

        // Handle direct blocks
        if (block_offset < D_BLOCK) {
            if (inode->blocks[block_offset] == 0) {
                break; // No more data
            }
            data_block = (char *)disks[0] + inode->blocks[block_offset];
        }
        // Handle indirect blocks
        else {
            size_t indirect_offset = block_offset - D_BLOCK;

            if (inode->blocks[IND_BLOCK] == 0) {
                break; // No indirect blocks
            }

            uint32_t *indirect_block = (uint32_t *)((char *)disks[0] + inode->blocks[IND_BLOCK]);
            if (indirect_block[indirect_offset] == 0) {
                break; // No more data
            }
            data_block = (char *)disks[0] + indirect_block[indirect_offset];
        }

        // Read from the block
        size_t block_available_space = BLOCK_SIZE - block_start_offset;
        size_t bytes_to_read = (size - bytes_read < block_available_space)
                                   ? size - bytes_read
                                   : block_available_space;

        memcpy(buf + bytes_read, (char *)data_block + block_start_offset, bytes_to_read);
        bytes_read += bytes_to_read;
        block_offset++;
        block_start_offset = 0; // Reset offset for subsequent blocks
    }

    return bytes_read; // Return the total number of bytes read
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode = get_inode(path, (char *)disks[0]);
    if (!inode) {
        return -ENOENT; // File not found
    }

    if (!S_ISREG(inode->mode)) {
        return -EISDIR; // Cannot write to a directory
    }

    size_t bytes_written = 0;               // Track how many bytes are written
    size_t block_offset = offset / BLOCK_SIZE;  // Determine starting block
    size_t block_start_offset = offset % BLOCK_SIZE; // Offset within the block

    while (bytes_written < size) {
	int disk_index, logical_block_num;

        if (superblock->raid_mode == 0) {
            // RAID 0 Striping
            disk_index = block_offset % diskNum;      // Determine disk for this block
            logical_block_num = block_offset / diskNum; // Logical block on the selected disk
        } else {
            // Non-RAID or RAID 1 (Mirroring)
            disk_index = 0;          // Default to the first disk
            logical_block_num = block_offset; // Logical block matches the block offset
        }

        if (block_offset < D_BLOCK) {
            // **Handle Direct Blocks**
            if (inode->blocks[block_offset] == 0) {
                // Allocate a new block
                int new_block = allocate_block((char *)disks[disk_index]);
                if (new_block < 0) {
                    return -ENOSPC; // No space available
                }

		if(superblock->raid_mode == 0) {
			for (int i = 0; i < diskNum; i++) {
			    char *disk = (char *)disks[i];
                            struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
                            mirror_inode->blocks[logical_block_num] = superblock->d_blocks_ptr + new_block * BLOCK_SIZE;
                        }
		} else {
                	// Mirror the allocation across all disks
                	for (int i = 0; i < diskNum; i++) {
                	    char *disk = (char *)disks[i];
                	    char *data_bitmap = disk + superblock->d_bitmap_ptr;
                	    data_bitmap[new_block / 8] |= (1 << (new_block % 8));

                	    struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
                	    mirror_inode->blocks[block_offset] = superblock->d_blocks_ptr + new_block * BLOCK_SIZE;
                	}
		}
            }

            // Write data to the block
            size_t block_available_space = BLOCK_SIZE - block_start_offset;
            size_t bytes_to_write = (size - bytes_written < block_available_space)
                                        ? size - bytes_written
                                        : block_available_space;

	    if(superblock->raid_mode == 0) {
                void *block_ptr = (char *)disks[disk_index] + inode->blocks[logical_block_num] + block_start_offset;
                memcpy(block_ptr, buf + bytes_written, bytes_to_write);
	    } else {
            	for (int i = 0; i < diskNum; i++) {
            	    char *disk = (char *)disks[i];
            	    void *block_ptr = (char *)disk + inode->blocks[block_offset] + block_start_offset;
            	    memcpy(block_ptr, buf + bytes_written, bytes_to_write);
            	}
	    }

            bytes_written += bytes_to_write;
            block_offset++;
            block_start_offset = 0; // Reset block offset for subsequent blocks
        } else {
            // **Handle Indirect Blocks**
            size_t indirect_offset = block_offset - D_BLOCK;

            if (inode->blocks[IND_BLOCK] == 0) {

                // Allocate an indirect block
                int indirect_block_index = allocate_block((char *)disks[disk_index]);
                if (indirect_block_index < 0) {
                    return -ENOSPC; // No space available
                }

		if(superblock->raid_mode == 0) {
			for (int i = 0; i < diskNum; i++) {
                            char *disk = (char *)disks[i];

                            void *indirect_block_ptr = disk + superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
                            memset(indirect_block_ptr, 0, BLOCK_SIZE); // Initialize the indirect block

                            struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
                            mirror_inode->blocks[IND_BLOCK] = superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
                        }

		} else {
                // Mirror the allocation across all disks
                	for (int i = 0; i < diskNum; i++) {
                	    char *disk = (char *)disks[i];
                	    char *data_bitmap = disk + superblock->d_bitmap_ptr;
                	    data_bitmap[indirect_block_index / 8] |= (1 << (indirect_block_index % 8));

                	    void *indirect_block_ptr = disk + superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
                	    memset(indirect_block_ptr, 0, BLOCK_SIZE); // Initialize the indirect block

                	    struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);
                	    mirror_inode->blocks[IND_BLOCK] = superblock->d_blocks_ptr + indirect_block_index * BLOCK_SIZE;
                	}
		}
            }

            uint32_t *indirect_block = (uint32_t *)((char *)disks[disk_index] + inode->blocks[IND_BLOCK]);

            if (indirect_block[indirect_offset] == 0) {
                // Allocate a new data block
                int new_block = allocate_block((char *)disks[0]);
                if (new_block < 0) {
                    return -ENOSPC; // No space available
                }

		if(superblock->raid_mode == 0) {
			for (int i = 0; i < diskNum; i++) {
                            char *disk = (char *)disks[i];

                            uint32_t *mirror_indirect_block = (uint32_t *)(disk + inode->blocks[IND_BLOCK]);
                            mirror_indirect_block[indirect_offset] = superblock->d_blocks_ptr + new_block * BLOCK_SIZE;
                        }
		} else {
                // Mirror the allocation across all disks
                	for (int i = 0; i < diskNum; i++) {
                	    char *disk = (char *)disks[i];
                	    char *data_bitmap = disk + superblock->d_bitmap_ptr;
                	    data_bitmap[new_block / 8] |= (1 << (new_block % 8));

                	    uint32_t *mirror_indirect_block = (uint32_t *)(disk + inode->blocks[IND_BLOCK]);
                	    mirror_indirect_block[indirect_offset] = superblock->d_blocks_ptr + new_block * BLOCK_SIZE;
                	}
		}
            }

            // Write data to the block via the indirect block pointer
            size_t block_available_space = BLOCK_SIZE - block_start_offset;
            size_t bytes_to_write = (size - bytes_written < block_available_space)
                                        ? size - bytes_written
                                        : block_available_space;

	    if(superblock->raid_mode == 0) {
		char *disk = (char *)disks[disk_index];
                void *block_ptr = (char *)disk + indirect_block[indirect_offset] + block_start_offset;
                memcpy(block_ptr, buf + bytes_written, bytes_to_write);
	    }
	    else {
	    	for (int i = 0; i < diskNum; i++) {
            	    char *disk = (char *)disks[i];
            	    void *block_ptr = (char *)disk + indirect_block[indirect_offset] + block_start_offset;
            	    memcpy(block_ptr, buf + bytes_written, bytes_to_write);
            	}
	    }

            bytes_written += bytes_to_write;
            block_offset++;
            block_start_offset = 0; // Reset block offset for subsequent blocks
        }
    }

    // Update inode metadata on all disks
    for (int i = 0; i < diskNum; i++) {
        char *disk = (char *)disks[i];
        struct wfs_inode *mirror_inode = (struct wfs_inode *)(disk + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE);

        // Update size and modification time
        mirror_inode->size = (offset + size > mirror_inode->size) ? offset + size : mirror_inode->size;
        mirror_inode->mtim = time(NULL);
    }

    return bytes_written; // Return the total number of bytes written
}

static int wfs_unlink(const char *path) {
    printf("unlink called for path: %s\n", path);

    // // Step 1: Extract parent directory and filename
    // char parent_path[1024], file_name[MAX_NAME];
    // strncpy(parent_path, path, sizeof(parent_path));
    // char *slash = strrchr(parent_path, '/');
    // if (!slash || slash == parent_path) {
    //     strncpy(file_name, path + 1, sizeof(file_name));
    //     strcpy(parent_path, "/");
    // } else {
    //     strncpy(file_name, slash + 1, sizeof(file_name));
    //     *slash = '\0';
    // }

    // // Step 2: Get the parent directory's inode
    // struct wfs_inode *parent_inode = get_inode(parent_path, (char *)disks[0]);
    // if (!parent_inode || !S_ISDIR(parent_inode->mode)) {
    //     return -ENOENT; // Parent directory not found
    // }

    // // Step 3: Locate the file in the parent directory
    // struct wfs_dentry *entry = NULL;
    // for (int i = 0; i < D_BLOCK; i++) {
    //     if (parent_inode->blocks[i] == 0) break;

    //     struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[0] + parent_inode->blocks[i]);
    //     for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
    //         if (strcmp(dir_entries[j].name, file_name) == 0) {
    //             entry = &dir_entries[j];
    //             break;
    //         }
    //     }
    //     if (entry) break;
    // }

    // if (!entry) return -ENOENT; // File not found

    // int file_inode_num = entry->num;

    // // Step 4: Free data blocks
    // for (int d = 0; d < diskNum; d++) {
    //     struct wfs_inode *file_inode = (struct wfs_inode *)((char *)disks[d] + superblock->i_blocks_ptr + file_inode_num * BLOCK_SIZE);

    //     for (int i = 0; i < D_BLOCK; i++) {
    //         if (file_inode->blocks[i] == 0) continue;

    //         int block_index = (file_inode->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE;
    //         char *data_bitmap = (char *)disks[d] + superblock->d_bitmap_ptr;
    //         data_bitmap[block_index / 8] &= ~(1 << (block_index % 8)); // Free block
    //         file_inode->blocks[i] = 0; // Clear block reference
    //     }

    //     // Free indirect blocks if any
    //     if (file_inode->blocks[IND_BLOCK] != 0) {
    //         uint32_t *indirect_blocks = (uint32_t *)((char *)disks[d] + file_inode->blocks[IND_BLOCK]);
    //         for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++) {
    //             if (indirect_blocks[i] == 0) break;

    //             int block_index = (indirect_blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE;
    //             char *data_bitmap = (char *)disks[d] + superblock->d_bitmap_ptr;
    //             data_bitmap[block_index / 8] &= ~(1 << (block_index % 8)); // Free block
    //         }

    //         // Free the indirect block itself
    //         int indirect_block_index = (file_inode->blocks[IND_BLOCK] - superblock->d_blocks_ptr) / BLOCK_SIZE;
    //         char *data_bitmap = (char *)disks[d] + superblock->d_bitmap_ptr;
    //         data_bitmap[indirect_block_index / 8] &= ~(1 << (indirect_block_index % 8));
    //         file_inode->blocks[IND_BLOCK] = 0;
    //     }

    //     // Free inode
    //     char *inode_bitmap = (char *)disks[d] + superblock->i_bitmap_ptr;
    //     inode_bitmap[file_inode_num / 8] &= ~(1 << (file_inode_num % 8));
    //     memset(file_inode, 0, BLOCK_SIZE); // Clear inode
    // }

    // // Step 5: Remove the directory entry
    // for (int d = 0; d < diskNum; d++) {
    //     struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[d] + parent_inode->blocks[0]);
    //     for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
    //         if (strcmp(dir_entries[j].name, file_name) == 0) {
    //             memset(&dir_entries[j], 0, sizeof(struct wfs_dentry)); // Clear directory entry
    //             break;
    //         }
    //     }
    // }

    // // Update parent directory metadata across all disks
    // for (int d = 0; d < diskNum; d++) {
    //     struct wfs_inode *mirror_parent_inode = (struct wfs_inode *)((char *)disks[d] + superblock->i_blocks_ptr + parent_inode->num * BLOCK_SIZE);
    //     mirror_parent_inode->size -= sizeof(struct wfs_dentry);
    //     mirror_parent_inode->mtim = time(NULL);
    // }

    return SUCCEED; // SUCCEED
}

static int wfs_rmdir(const char *path) {
    printf("rmdir called for path: %s\n", path);

    // // Step 1: Extract parent directory and target directory name
    // char parent_path[1024], dir_name[MAX_NAME];
    // strncpy(parent_path, path, sizeof(parent_path));
    // char *slash = strrchr(parent_path, '/');
    // if (!slash || slash == parent_path) {
    //     strncpy(dir_name, path + 1, sizeof(dir_name));
    //     strcpy(parent_path, "/");
    // } else {
    //     strncpy(dir_name, slash + 1, sizeof(dir_name));
    //     *slash = '\0';
    // }

    // // Step 2: Get the parent directory's inode
    // struct wfs_inode *parent_inode = get_inode(parent_path, (char *)disks[0]);
    // if (!parent_inode || !S_ISDIR(parent_inode->mode)) {
    //     return -ENOENT; // Parent directory not found
    // }

    // // Step 3: Locate the target directory in the parent directory
    // struct wfs_dentry *entry = NULL;
    // for (int i = 0; i < D_BLOCK; i++) {
    //     if (parent_inode->blocks[i] == 0) break;

    //     struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[0] + parent_inode->blocks[i]);
    //     for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
    //         if (strcmp(dir_entries[j].name, dir_name) == 0) {
    //             entry = &dir_entries[j];
    //             break;
    //         }
    //     }
    //     if (entry) break;
    // }

    // if (!entry) return -ENOENT; // Directory not found

    // int dir_inode_num = entry->num;
    // struct wfs_inode *dir_inode = (struct wfs_inode *)((char *)disks[0] + superblock->i_blocks_ptr + dir_inode_num * BLOCK_SIZE);

    // // Step 4: Check that the directory is empty
    // for (int i = 0; i < D_BLOCK; i++) {
    //     if (dir_inode->blocks[i] == 0) continue;

    //     struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[0] + dir_inode->blocks[i]);
    //     for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
    //         if (dir_entries[j].num != 0 && strcmp(dir_entries[j].name, ".") != 0 && strcmp(dir_entries[j].name, "..") != 0) {
    //             return -ENOTEMPTY; // Directory is not empty
    //         }
    //     }
    // }

    // // Step 5: Free directory blocks and inode
    // for (int d = 0; d < diskNum; d++) {
    //     struct wfs_inode *mirror_dir_inode = (struct wfs_inode *)((char *)disks[d] + superblock->i_blocks_ptr + dir_inode_num * BLOCK_SIZE);

    //     for (int i = 0; i < D_BLOCK; i++) {
    //         if (mirror_dir_inode->blocks[i] == 0) continue;

    //         int block_index = (mirror_dir_inode->blocks[i] - superblock->d_blocks_ptr) / BLOCK_SIZE;
    //         if (superblock->raid_mode == 0 && block_index % diskNum != d) continue; // Skip non-relevant disks for RAID 0

    //         char *data_bitmap = (char *)disks[d] + superblock->d_bitmap_ptr;
    //         data_bitmap[block_index / 8] &= ~(1 << (block_index % 8));
    //         mirror_dir_inode->blocks[i] = 0; // Clear block reference
    //     }

    //     // Free inode
    //     char *inode_bitmap = (char *)disks[d] + superblock->i_bitmap_ptr;
    //     inode_bitmap[dir_inode_num / 8] &= ~(1 << (dir_inode_num % 8));
    //     memset(mirror_dir_inode, 0, BLOCK_SIZE);
    // }

    // // Step 6: Remove the directory entry from the parent directory
    // for (int d = 0; d < diskNum; d++) {
    //     struct wfs_dentry *dir_entries = (struct wfs_dentry *)((char *)disks[d] + parent_inode->blocks[0]);
    //     for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
    //         if (strcmp(dir_entries[j].name, dir_name) == 0) {
    //             memset(&dir_entries[j], 0, sizeof(struct wfs_dentry)); // Clear directory entry
    //             break;
    //         }
    //     }
    // }

    // // Step 7: Update parent directory metadata across all disks
    // for (int d = 0; d < diskNum; d++) {
    //     struct wfs_inode *mirror_parent_inode = (struct wfs_inode *)((char *)disks[d] + superblock->i_blocks_ptr + parent_inode->num * BLOCK_SIZE);
    //     mirror_parent_inode->size -= sizeof(struct wfs_dentry);
    //     mirror_parent_inode->mtim = time(NULL);
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

