#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include "wfs.h"

struct wfsSbExtended {
    struct wfs_sb base;
    int raid_mode;
    int num_disks;
} __attribute__((packed));

int main(int argc, char *argv[]) {
    int opt;
    int raid_mode = -1;
    int num_inodes = -1;
    int num_blocks = -1;
    char* disk_files[256] = {NULL};
    int num_disks = 0;

    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        //switch (opt) {
            if (opt == 'r'){
                if (strcmp(optarg, "0") == 0) {
                    raid_mode = 0;
                }
                else if (strcmp(optarg, "1") == 0) {
                    raid_mode = 1;
                }
                else if (strcmp(optarg, "1v") == 0) {
                    raid_mode = 2;
                }
                else {
                    return 1;
                }
            }
            if (opt == 'd'){
                disk_files[num_disks++] = optarg;
            }
            if (opt == 'i'){
                num_inodes = atoi(optarg);
            }
            if (opt == 'b'){
                num_blocks = atoi(optarg);
            }
        //}
    }

    // Error checking
    if (raid_mode == -1 || num_disks < 2 || num_inodes <= 0 || num_blocks <= 0) {
        return 1;
    }

    // Round both inodes and blocks to multiples of 32
    num_inodes = ((num_inodes + 31) / 32) * 32;;
    num_blocks = ((num_blocks + 31) / 32) * 32;;

    // Calculate bitmap sizes in bytes
    size_t inode_bitmap_bytes = (num_inodes + 7) / 8;
    size_t data_bitmap_bytes = (num_blocks + 7) / 8;

    // Calculate offsets
    size_t superblock_size = sizeof(struct wfsSbExtended);
    off_t i_bitmap_off = superblock_size;
    off_t d_bitmap_off = i_bitmap_off + inode_bitmap_bytes;
    off_t i_start = d_bitmap_off + data_bitmap_bytes;
    
    // Each inode must be at a location divisible by BLOCK_SIZE
    i_start = ((i_start + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
    
    // Calculate data blocks start
    off_t d_start = i_start + (num_inodes * BLOCK_SIZE);

    // Initialize superblock
    struct wfsSbExtended sb = {
        .base = {
            .num_inodes = num_inodes,
            .num_data_blocks = num_blocks,
            .i_bitmap_ptr = i_bitmap_off,
            .d_bitmap_ptr = d_bitmap_off,
            .i_blocks_ptr = i_start,
            .d_blocks_ptr = d_start
        },
        .raid_mode = raid_mode,
        .num_disks = num_disks
    };

    // Calculate total size needed
    size_t fs_size = d_start + (num_blocks * BLOCK_SIZE);

    // Open all disks and map them
    int* fds = malloc(num_disks * sizeof(int));
    void** disk_maps = malloc(num_disks * sizeof(void*));

    for (int i = 0; i < num_disks; i++) {
        fds[i] = open(disk_files[i], O_RDWR);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) {
                munmap(disk_maps[j], fs_size);
                close(fds[j]);
            }
            free(fds);
            free(disk_maps);
            return -1;  // Runtime error - file operation failed
        }

        struct stat st;
        if (fstat(fds[i], &st) < 0 || st.st_size < fs_size) {
            for (int j = 0; j < i; j++) {
                munmap(disk_maps[j], fs_size);
                close(fds[j]);
            }
            close(fds[i]);
            free(fds);
            free(disk_maps);
            return -1;  // Runtime error - file too small or stat failed
        }

        // Map the entire disk
        disk_maps[i] = mmap(NULL, fs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (disk_maps[i] == MAP_FAILED) {
            for (int j = 0; j < i; j++) {
                munmap(disk_maps[j], fs_size);
                close(fds[j]);
            }
            close(fds[i]);
            free(fds);
            free(disk_maps);
            return -1;  // Runtime error - mapping failed
        }

        // Zero out the entire disk
        memset(disk_maps[i], 0, fs_size);

        // Write superblock
        memcpy(disk_maps[i], &sb, sizeof(sb));

        // Write inode bitmap
        char* inode_bitmap = (char*)disk_maps[i] + i_bitmap_off;
        memset(inode_bitmap, 0, inode_bitmap_bytes);
        inode_bitmap[0] = 1;  // Mark first inode as used

        // Write data bitmap
        char* data_bitmap = (char*)disk_maps[i] + d_bitmap_off;
        memset(data_bitmap, 0, data_bitmap_bytes);

        // Write root inode
        struct wfs_inode root = {0};
        root.mode = S_IFDIR | 0755;
        root.uid = getuid();
        root.gid = getgid();
        root.nlinks = 2;
        root.atim = time(NULL);
        root.mtim = time(NULL);
        root.ctim = time(NULL);
        memcpy((char*)disk_maps[i] + i_start, &root, sizeof(root));

        // Sync changes to disk
        msync(disk_maps[i], fs_size, MS_SYNC);
    }

    // Cleanup
    for (int i = 0; i < num_disks; i++) {
        munmap(disk_maps[i], fs_size);
        close(fds[i]);
    }
    free(fds);
    free(disk_maps);
    return 0;  // Success - everything worked
}