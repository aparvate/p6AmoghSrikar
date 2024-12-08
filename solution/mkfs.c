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

#define ALIGN_UP(x, align) (((x) + ((align)-1)) & ~((align)-1))
#define ROUND_UP(x, align) (((x) + ((align)-1)) & ~((align)-1))

int raidNum = -1;
char *disks[10];
int diskNum = 0;
int nodeNum = 0;
int blockNum = 0;

void print_usage() {
    fprintf(stderr, "Usage: mkfs -r <mode> -d <disk> ... -i <num_inodes> -b <num_blocks>\n");
    exit(1);
}

void initialize_disk(const char *disk, size_t total_size, struct wfs_sb *sb) {
    int fd = open(disk, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Error opening disk file");
        exit(-1);
    }

    if (ftruncate(fd, total_size) < 0) {
        perror("Error resizing disk file");
        close(fd);
        exit(-1);
    }

    void *disk_map = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk_map == MAP_FAILED) {
        perror("Error mapping disk file");
        close(fd);
        exit(-1);
    }

    memcpy(disk_map, sb, BLOCK_SIZE); // Write superblock

    munmap(disk_map, total_size);
    close(fd);
}

void create_filesystem() {
    // Round up the number of data blocks and inodes to the nearest multiple of 32
    blockNum = ROUND_UP(blockNum, 32);
    nodeNum = ROUND_UP(nodeNum, 32);

    // Align the inode region to block size
    size_t inodes_size = ROUND_UP(nodeNum * BLOCK_SIZE, BLOCK_SIZE);

    // Calculate sizes
    size_t inode_bitmap_size = (nodeNum + 7) / 8; // Each inode uses 1 bit
    size_t data_bitmap_size = (blockNum + 7) / 8; // Each block uses 1 bit
    size_t superblock_size = sizeof(struct wfs_sb);

    // Calculate offsets
    size_t superblock_offset = 0;
    size_t inode_bitmap_offset = superblock_offset + superblock_size;
    size_t data_bitmap_offset = inode_bitmap_offset + inode_bitmap_size;
    size_t inodes_offset = ROUND_UP(data_bitmap_offset + data_bitmap_size, BLOCK_SIZE); // Block-aligned
    size_t data_blocks_offset = inodes_offset + inodes_size;

    // Total filesystem size
    size_t total_fs_size = data_blocks_offset + (blockNum * BLOCK_SIZE);

    struct wfs_sb sb;
    memset(&sb, 0, sizeof(struct wfs_sb));

    sb.num_inodes = nodeNum;
    sb.num_data_blocks = blockNum;

    sb.i_bitmap_ptr = inode_bitmap_offset;
    sb.d_bitmap_ptr = data_bitmap_offset;
    sb.i_blocks_ptr = inodes_offset;
    sb.d_blocks_ptr = data_blocks_offset;

    sb.raid_mode = raidNum;
    sb.num_disks = diskNum;

    //printf("Filesystem Layout:\n");
    //printf("  Superblock Offset: %zu bytes\n", superblock_offset);
    //printf("  Inode Bitmap Offset: %zu bytes (size: %zu bytes)\n", inode_bitmap_offset, inode_bitmap_size);
    //printf("  Data Bitmap Offset: %zu bytes (size: %zu bytes)\n", data_bitmap_offset, data_bitmap_size);
    //printf("  Inodes Offset: %zu bytes (size: %zu bytes)\n", inodes_offset, inodes_size);
    //printf("  Data Blocks Offset: %zu bytes\n", data_blocks_offset);
    //printf("  Total Filesystem Size: %zu bytes\n", total_fs_size);

    // Check if total size fits on available disks
    //size_t available_disk_space = num_disks * 1024 * 1024; // Assuming 1MB per disk
    //size_t available_disk_space = 0;
    for (int i = 0; i < diskNum; i++) {
        struct stat st;
        if (stat(disks[i], &st) != 0) {
            perror("Error getting disk size");
            exit(-1); // Return 255 if stat fails
        }
	if((size_t)st.st_size < total_fs_size) {
		exit(-1);
	}
        //available_disk_space += st.st_size; // Add the size of each disk
    }
    //printf("total_fs_size: %ld\n", total_fs_size);
    //printf("available disk space: %ld\n", available_disk_space);

    //if (total_fs_size > available_disk_space) {
    //    fprintf(stderr, "Error: Disk size is too small for the specified layout.\n");
    //    exit(-1);
    //}

    for (int i = 0; i < diskNum; i++) {
        initialize_disk(disks[i], total_fs_size, &sb);

        int fd = open(disks[i], O_RDWR);
        if (fd < 0) {
            perror("Error reopening disk file");
            exit(-1);
        }

        void *disk_map = mmap(NULL, total_fs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_map == MAP_FAILED) {
            perror("Error mapping disk file");
            close(fd);
            exit(-1);
        }

        // Initialize bitmaps
        char *inode_bitmap = (char *)disk_map + sb.i_bitmap_ptr;
        memset(inode_bitmap, 0, inode_bitmap_size);
        inode_bitmap[0] |= 0x01; // Mark root inode (Inode 0) as allocated

        char *data_bitmap = (char *)disk_map + sb.d_bitmap_ptr;
        memset(data_bitmap, 0, data_bitmap_size);

        // Initialize inode table
        struct wfs_inode *inode_table = (struct wfs_inode *)((char *)disk_map + sb.i_blocks_ptr);
        memset(inode_table, 0, nodeNum * sizeof(struct wfs_inode));

        // Root inode initialization
        inode_table[0].mode = S_IFDIR | 0755;
        inode_table[0].nlinks = 2;
        inode_table[0].size = 0;
        inode_table[0].atim = inode_table[0].mtim = inode_table[0].ctim = time(NULL);
        inode_table[0].uid = getuid();
        inode_table[0].gid = getgid();

        if (msync(disk_map, total_fs_size, MS_SYNC) < 0) {
            perror("Error syncing disk map");
            munmap(disk_map, total_fs_size);
            close(fd);
            exit(-1);
        }

        munmap(disk_map, total_fs_size);
        close(fd);
    }
}

int main(int argc, char *argv[]) {
    int opt;
    raidNum = -1;
    nodeNum = -1;
    blockNum = -1;
    diskNum = 0;

    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        if (opt == 'r'){
            if (strcmp(optarg, "0") == 0) {
                raidNum = 0;
            }
            else if (strcmp(optarg, "1") == 0) {
                raidNum = 1;
            }
            else if (strcmp(optarg, "1v") == 0) {
                raidNum = 2;
            }
            else {
                return 1;
            }
        }
        if (opt == 'd'){
            disks[diskNum++] = optarg;
        }
        if (opt == 'i'){
            nodeNum = atoi(optarg);
        }
        if (opt == 'b'){
            blockNum = atoi(optarg);
        }
    }

    //Errors
    if (raidNum == -1 || diskNum < 2 || nodeNum <= 0 || blockNum <= 0) {
        return 1;
    }
    create_filesystem();
    //printf("Filesystem created successfully in RAID mode %d with %d disks.\n", raid_mode, num_disks);
    return 0;
}


// wfs.h file

#include <time.h>
#include <sys/stat.h>

#define BLOCK_SIZE (512)
#define MAX_NAME   (28)

#define D_BLOCK    (6)
#define IND_BLOCK  (D_BLOCK+1)
#define N_BLOCKS   (IND_BLOCK+1)