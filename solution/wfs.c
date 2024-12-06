#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include "wfs.h"

void *mapped_region;
struct wfs_sb *superblock;

int my_getattr() {
    printf("hello1\n");
    return 0;
}

int my_mknod() {
    printf("hello2\n");
    return 0;
}

int my_mkdir() {
    printf("hai3\n");
    return 0;
}

int my_unlink() {
    printf("hello4\n");
    return 0;
}

int my_rmdir() {
    printf("hello5\n");
    return 0;
}

int my_read() {
    printf("hello6\n");
    return 0;
}

int my_write() {
    printf("hello7\n");
    return 0;
}

int my_readdir() {
    printf("hello8\n");
    return 0;
}


static struct fuse_operations ops = {
    .getattr = my_getattr,
    .mknod   = my_mknod,
    .mkdir   = my_mkdir,
    .unlink  = my_unlink,
    .rmdir   = my_rmdir,
    .read    = my_read,
    .write   = my_write,
    .readdir = my_readdir,
};



int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk1> <disk2> ... <mount_point> [FUSE options]\n", argv[0]);
        return -1;
    }

    printf("ARGV ORIGINALLY\n");
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d]: %s\n", i, argv[i]);
    }

    // Identify disk image arguments
    int num_disks = 0;
    while (num_disks + 1 < argc && argv[num_disks + 1][0] != '-') {
        num_disks++;
    }

    if (num_disks < 2) {
        fprintf(stderr, "Error: At least two disk images are required.\n");
        return -1;
    }

    // Debug: Print disk images
    printf("Disk images:\n");
    for (int i = 0; i < num_disks; i++) {
        printf("  Disk %d: %s\n", i + 1, argv[i + 1]);
    }

    // Create a new array for FUSE arguments
    int fuse_argc = argc - num_disks - 1; // Exclude disk images and argv[0]
    char **fuse_argv = malloc(fuse_argc * sizeof(char *));
    if (!fuse_argv) {
        perror("Error allocating memory for FUSE arguments");
        return -1;
    }

    // Copy FUSE-related arguments to fuse_argv (skip argv[0] and disk image paths)
    for (int i = num_disks + 1; i < argc; i++) {
        fuse_argv[i - num_disks - 1] = argv[i];
    }

    // Debug: Print updated argc and argv for FUSE
    printf("FUSE argc: %d\n", fuse_argc);
    printf("FUSE argv:\n");
    for (int i = 0; i < fuse_argc; i++) {
        printf("  argv[%d]: %s\n", i, fuse_argv[i]);
    }

    // Start FUSE
    int result = fuse_main(fuse_argc, fuse_argv, &ops, NULL);

    // Free allocated memory
    free(fuse_argv);

    return result;
}
