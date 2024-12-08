//MKFS File

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include "wfs.h"

#define ALIGN_UP(x, align) (((x) + ((align)-1)) & ~((align)-1))
#define ROUND_UP(x, align) (((x) + ((align)-1)) & ~((align)-1))

int raid_mode = -1;
char *disk_files[10];
int num_disks = 0;
int num_inodes = 0;
int num_data_blocks = 0;

void print_usage() {
    fprintf(stderr, "Usage: mkfs -r <mode> -d <disk> ... -i <num_inodes> -b <num_blocks>\n");
    exit(1);
}

void parse_arguments(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        switch (opt) {
        case 'r':
            if (strcmp(optarg, "0") == 0) raid_mode = 0;
            else if (strcmp(optarg, "1") == 0) raid_mode = 1;
            else if (strcmp(optarg, "1v") == 0) raid_mode = 2;
            else {
                fprintf(stderr, "Error: Invalid RAID mode\n");
                exit(1);
            }
            break;
        case 'd':
            if (num_disks >= 10) {
                fprintf(stderr, "Error: Too many disks\n");
                exit(1);
            }
            disk_files[num_disks++] = optarg;
            break;
        case 'i':
            num_inodes = atoi(optarg);
            break;
        case 'b':
            num_data_blocks = atoi(optarg);
            break;
        default:
            print_usage();
        }
    }

    if (raid_mode == -1) {
        fprintf(stderr, "Error: No RAID mode specified\n");
        exit(1);
    }
    if (num_disks < 2) {
        fprintf(stderr, "Error: At least 2 disks are required\n");
        exit(1);
    }
    if (num_inodes <= 0 || num_data_blocks <= 0) {
        fprintf(stderr, "Error: Invalid number of inodes or data blocks\n");
        exit(1);
    }
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
    num_data_blocks = ROUND_UP(num_data_blocks, 32);
    num_inodes = ROUND_UP(num_inodes, 32);

    // Align the inode region to block size
    size_t inodes_size = ROUND_UP(num_inodes * BLOCK_SIZE, BLOCK_SIZE);

    // Calculate sizes
    size_t inode_bitmap_size = (num_inodes + 7) / 8; // Each inode uses 1 bit
    size_t data_bitmap_size = (num_data_blocks + 7) / 8; // Each block uses 1 bit
    size_t superblock_size = sizeof(struct wfs_sb);

    // Calculate offsets
    size_t superblock_offset = 0;
    size_t inode_bitmap_offset = superblock_offset + superblock_size;
    size_t data_bitmap_offset = inode_bitmap_offset + inode_bitmap_size;
    size_t inodes_offset = ROUND_UP(data_bitmap_offset + data_bitmap_size, BLOCK_SIZE); // Block-aligned
    size_t data_blocks_offset = inodes_offset + inodes_size;

    // Total filesystem size
    size_t total_fs_size = data_blocks_offset + (num_data_blocks * BLOCK_SIZE);

    struct wfs_sb sb;
    memset(&sb, 0, sizeof(struct wfs_sb));

    sb.num_inodes = num_inodes;
    sb.num_data_blocks = num_data_blocks;

    sb.i_bitmap_ptr = inode_bitmap_offset;
    sb.d_bitmap_ptr = data_bitmap_offset;
    sb.i_blocks_ptr = inodes_offset;
    sb.d_blocks_ptr = data_blocks_offset;

    sb.raid_mode = raid_mode;
    sb.num_disks = num_disks;

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
    for (int i = 0; i < num_disks; i++) {
        struct stat st;
        if (stat(disk_files[i], &st) != 0) {
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

    for (int i = 0; i < num_disks; i++) {
        initialize_disk(disk_files[i], total_fs_size, &sb);

        int fd = open(disk_files[i], O_RDWR);
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
        memset(inode_table, 0, num_inodes * sizeof(struct wfs_inode));

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
    parse_arguments(argc, argv);
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