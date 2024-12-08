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

void freeFunc(int* fds, void** diskMapStore){
    free(fds);
    free(diskMapStore);
}

int main(int argc, char *argv[]) {
    int opt;
    int raidNum = -1;
    int nodeNum = -1;
    int blockNum = -1;
    char* disks[256] = {NULL};
    int diskNum = 0;

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

    //Sizes
    blockNum = ((blockNum + 31) / 32) * 32;
    nodeNum = ((nodeNum + 31) / 32) * 32;
    size_t dataSize = (blockNum + 7) / 8;
    size_t nodeSize = (nodeNum + 7) / 8;

    //Offsets
    off_t iOff = sizeof(struct wfs_sb);
    off_t dOff = iOff + nodeSize;
    off_t iStart = (((dOff + dataSize) + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;

    //Size
    size_t fsSize = (iStart + (nodeNum * BLOCK_SIZE)) + (blockNum * BLOCK_SIZE);

    //Blocks
    struct wfs_sb sb = {
        .num_inodes = nodeNum,
        .num_data_blocks = blockNum,
        .i_bitmap_ptr = iOff,
        .d_bitmap_ptr = dOff,
        .i_blocks_ptr = iStart,
        .d_blocks_ptr = (iStart + (nodeNum * BLOCK_SIZE)),
        .raid_mode = raidNum,
        .num_disks = diskNum
    };

    int* fds = malloc(diskNum * sizeof(int));
    void** diskMapStore = malloc(diskNum * sizeof(void*));

    for (int i = 0; i < diskNum; i++) {
        fds[i] = open(disks[i], O_RDWR);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) {
                munmap(diskMapStore[j], fsSize);
                close(fds[j]);
            }
            freeFunc(fds, diskMapStore);
            return -1;
        }

        struct stat st;
        if (fstat(fds[i], &st) < 0 || st.st_size < fsSize) {
            for (int j = 0; j < i; j++) {
                munmap(diskMapStore[j], fsSize);
                close(fds[j]);
            }
            close(fds[i]);
            freeFunc(fds, diskMapStore);
            return -1;
        }

        diskMapStore[i] = mmap(NULL, fsSize, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (mmap(NULL, fsSize, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0) == MAP_FAILED) {
            for (int j = 0; j < i; j++) {
                munmap(diskMapStore[j], fsSize);
                close(fds[j]);
            }
            close(fds[i]);
            freeFunc(fds, diskMapStore);
            return -1;
        }
        // Writing
        memset(diskMapStore[i], 0, fsSize);
        memcpy(diskMapStore[i], &sb, sizeof(sb));
        char* nodeMapSize = (char*)diskMapStore[i] + iOff;
        memset(nodeMapSize, 0, nodeSize);
        nodeMapSize[0] = 1;
        // Root
        struct wfs_inode root = {0};
        root.mode = S_IFDIR | 0755;
        root.uid = getuid();
        root.gid = getgid();
        root.nlinks = 2;
        root.atim = time(NULL);
        root.mtim = time(NULL);
        root.ctim = time(NULL);
        root.num = 0;
        memcpy((char*)diskMapStore[i] + iStart, &root, sizeof(root));

        msync(diskMapStore[i], fsSize, MS_SYNC);
    }
    // Cleaning
    for (int i = 0; i < diskNum; i++) {
        munmap(diskMapStore[i], fsSize);
        close(fds[i]);
    }
    freeFunc(fds, diskMapStore);
    return 0;
}