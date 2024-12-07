#include "stdlib.h"
#include "stdio.h"
#include "sys/stat.h"
#include "wfs.h"
#include "unistd.h"
#define ALLOCATE_SUPER_BLOCK() (struct wfs_sb *)malloc(sizeof(struct wfs_sb))
#define WFS_SB_SIZE (sizeof(struct wfs_sb))
#define WFS_INODE_SIZE (sizeof(struct wfs_inode))

int main(int argc, char *argv[]) {
    if (argc != 7) {
        exit(1);
    }
    FILE *disk = fopen(argv[2], "r+");
    if (disk == NULL) {
        exit(1);
    }
    int inode = atoi(argv[4]);
    int blocks = atoi(argv[6]);
    inode = (inode + 31) / 32 * 32;
    blocks = (blocks + 31) / 32 * 32;
    fseek(disk, 0, SEEK_END);
    int size_disk = ftell(disk);
    int size = blocks * BLOCK_SIZE;
    if ((size + (inode * BLOCK_SIZE) + (int)inode + (int)blocks + (int)WFS_SB_SIZE) > size_disk) {
        exit(1);
    }
    struct wfs_sb *super_block = ALLOCATE_SUPER_BLOCK();
    if (super_block == NULL) {
            exit(1);
    }
    super_block->num_inodes = inode;
    super_block->num_data_blocks = blocks;
    super_block->i_bitmap_ptr = 0x0 + WFS_SB_SIZE;
    super_block->d_bitmap_ptr = super_block->i_bitmap_ptr + (inode/8);
    super_block->i_blocks_ptr = super_block->d_bitmap_ptr + (blocks/8);
    super_block->d_blocks_ptr = super_block->i_blocks_ptr + BLOCK_SIZE * inode;
    fseek(disk, 0, SEEK_SET);
    fwrite(super_block, WFS_SB_SIZE, 1, disk);
    struct wfs_inode *root = malloc(WFS_INODE_SIZE);
    if (root == NULL) {
        exit(1);
    }
    root->num = 0;
    root->mode = __S_IFDIR | S_IRWXU;
    root->uid = getuid();
    root->gid = getgid();
    root->size = 0;
    root->nlinks = 0;
    root->atim = time(NULL);
    root->mtim = time(NULL);
    root->ctim = time(NULL);
    for (int i = 0; i < 7; i++) {
        root->blocks[i] = 0;
    }
    fseek(disk, super_block->i_blocks_ptr, SEEK_SET);
    fwrite(root, WFS_INODE_SIZE, 1, disk);
    fseek(disk, super_block->i_bitmap_ptr, SEEK_SET);
    int bit_map;
    fread(&bit_map, super_block->d_bitmap_ptr - super_block->i_bitmap_ptr, 1, disk);
    bit_map = 1;
    fseek(disk, super_block->i_bitmap_ptr, SEEK_SET);
    fwrite(&bit_map, super_block->d_bitmap_ptr - super_block->i_bitmap_ptr, 1, disk);
    fclose(disk);
    free(root);
    free(super_block);
    return 0;
}