#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <libgen.h>
#include <assert.h>
#include <stdint.h>
#define min(a, b) ((a) < (b) ? (a) : (b))

void *mapped_memory_region;
int err;

int get_inode(struct wfs_inode *enclosing, char *path, struct wfs_inode **inode);
int get_inode_path(char *path, struct wfs_inode **inode);
int wfs_mknod(const char *path, mode_t mode, dev_t dev);
int wfs_mkdir(const char *path, mode_t mode);
int wfs_getattr(const char *path, struct stat *stbuf);
int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int wfs_unlink(const char *path);
int wfs_rmdir(const char *path);
int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);

char *find_offset(struct wfs_inode *inode, off_t off, int flag);
struct wfs_inode *find_inode(int num);

size_t allocate_block(uint32_t *bitmap, size_t size);
off_t allocate_DB();
struct wfs_inode *allocate_inode();

void free_bitmap(uint32_t pos, uint32_t *bitmap);
void free_db_block(off_t block);
void free_inode(struct wfs_inode *inode);

void change(struct stat *stbuf, struct wfs_inode *inode);
void update_size(struct wfs_inode *inode, off_t offset, size_t size);
char* mmap_ptr(off_t offset);

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    (void)dev; 
    struct wfs_inode *parent = NULL;
    char *file = strdup(path);
    char *dir = strdup(path);
    if (!file || !dir) {
        free(dir);
        free(file);
        return -ENOMEM;
    }
    if (get_inode_path(dirname(dir), &parent) < 0) {
        free(dir);
        free(file);
        return err;
    }
    struct wfs_inode *inode = allocate_inode();
    if (inode == NULL) {
        free(dir);
        free(file);
        return -ENOSPC;  
    }
    inode->mode = __S_IFREG | mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 1;
    struct wfs_dentry *entry;
    off_t offset = 0;
    bool check = false;
    while (offset < parent->size) {
        entry = (struct wfs_dentry *)find_offset(parent, offset, 0);
        if (entry != NULL) {
            if (entry->num == 0) {
                entry->num = inode->num;
                strncpy(entry->name, basename(file), MAX_NAME);
                parent->nlinks++;
                check = true;
                break;
            }
            offset = offset + sizeof(struct wfs_dentry);  
        } 
    }
    if (!check) {
        entry = (struct wfs_dentry *)find_offset(parent, parent->size, 1);
        if (entry != NULL) {
            entry->num = inode->num;
            strncpy(entry->name, basename(file), MAX_NAME);
            parent->nlinks++;
            parent->size = parent->size + sizeof(struct wfs_dentry);
        }
        else{
            free_inode(inode);
            free(dir);
            free(file);
            return -ENOSPC;
        }  
    }
    free(dir);
    free(file);
    return 0;
}

int wfs_mkdir(const char *path, mode_t mode) {
    struct wfs_inode *parent = NULL;
    char *dir = strdup(path);
    char *file = strdup(path);
    if(!file || !dir){
        free(dir);
        free(file);
        return -ENOMEM;
    }
    if (get_inode_path(dirname(dir), &parent) < 0) {
        free(dir);
        free(file);
        return err;
    }
    struct wfs_inode *inode = allocate_inode();
    if (inode == NULL) {
        free(dir);
        free(file);
        return -ENOSPC;
    }
    inode->mode = S_IFDIR | mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 2;
    struct wfs_dentry *entry;
    off_t offset = 0;
    bool check = false;
    while (offset < parent->size) {
        entry = (struct wfs_dentry *)find_offset(parent, offset, 0);
        if (entry != NULL) {
            if (entry->num == 0) {
            entry->num = inode->num;
            strncpy(entry->name, basename(file), MAX_NAME);
            parent->nlinks++;
            check = true;
            break;
        }
        }
        offset = offset + sizeof(struct wfs_dentry);
    }
    if (!check) {
        entry = (struct wfs_dentry *)find_offset(parent, parent->size, 1);
        if (entry == NULL) {
            free_inode(inode);
            free(dir);
            free(file);
            return -ENOSPC;
        }
        entry->num = inode->num;
        strncpy(entry->name, basename(file), MAX_NAME);
        parent->nlinks++;
        parent->size = parent->size + sizeof(struct wfs_dentry);
    }
    free(dir);
    free(file);
    return 0;
}

int wfs_getattr(const char *path, struct stat *stbuf) {
    struct wfs_inode *inode;
    char *temp = strdup(path);
    if (get_inode_path(temp, &inode) < 0){
        free(temp);
        return err;
    }
    change(stbuf, inode);
    free(temp);
    return 0;
}

void change(struct stat *stbuf, struct wfs_inode *inode) {
    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
}


int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
    (void)fi;
    struct wfs_inode *inode;
    char *search = strdup(path);
    if (get_inode_path(search, &inode) < 0){
        free(search);
        return err;
    }
    size_t read = 0;
    for (size_t i = 0; read < size && offset + read < inode->size; i++){
            size_t curr = offset + read;
            size_t block = BLOCK_SIZE - (curr % BLOCK_SIZE);
            size_t remaining = inode->size - curr;
            size_t read_now;
            if (block > remaining) {
                read_now = remaining;
            } else {
                read_now = block;
            }
            char *addr = find_offset(inode, curr, 0);
            if(addr == NULL) {
                return -ENOENT;
            }
            memcpy(buf + read, addr, read_now);
            read = read + read_now;
    }
    free(search);
    return read;
}

int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
    (void)fi;
    struct wfs_inode *inode;
    char *search = strdup(path);
    if (get_inode_path(search, &inode) < 0){
        free(search);
        return err;
    }
    size_t write = 0;
    size_t curr = offset;
    size_t write_now = 0;

    for (; write < size; write = write + write_now, curr = curr + write_now){
        size_t block = BLOCK_SIZE - (curr % BLOCK_SIZE);
        write_now = min(block, size - write);
        char *addr = find_offset(inode, curr, 1);
        if (addr == NULL){
            free(search);
            return -ENOSPC; 
        }
        memcpy(addr, buf + write, write_now);
    }
    update_size(inode, offset, size);
    free(search);
    return write;
}

void update_size(struct wfs_inode *inode, off_t offset, size_t size) {
    if (offset + size > inode->size) {
        inode->size = offset + size;
    }
}

int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
    (void)fi;
    (void)offset;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    struct wfs_inode *inode;
    char *search = strdup(path);
    if (get_inode_path(search, &inode) < 0) {
        free(search);
        return err;
    }
    size_t size = inode->size;
    struct wfs_dentry *entry;
    off_t offset_temp = 0;
    while (offset_temp < size) {
        entry = (struct wfs_dentry *)find_offset(inode, offset_temp, 0);
        if (entry != NULL) {
            if (entry->num != 0) {
                filler(buf, entry->name, NULL, 0);
            }
        }
        offset_temp = offset_temp + sizeof(struct wfs_dentry);
    }
    free(search);
    return 0;
}

int wfs_unlink(const char *path) {
    struct wfs_inode *parent;
    struct wfs_inode *inode;
    char *path_copy = strdup(path);
    char *search = strdup(path);
    if (get_inode_path(dirname(path_copy), &parent) < 0) {
        free(path_copy);
        free(search);
        return err;
    }
    if (get_inode_path(search, &inode) < 0) {
        free(path_copy);
        free(search);
        return err;
    }
    size_t size = parent->size;
    struct wfs_dentry *entry;
    off_t offset = 0;
    while (offset < size) {
        entry = (struct wfs_dentry *)find_offset(parent, offset, 0);
        if(entry == NULL) {
            return -ENOENT;
        }
        if (entry->num == inode->num) {
            entry->num = 0;
            break;
        }
        offset = offset + sizeof(struct wfs_dentry);
    }
    for (int i = 0; i < D_BLOCK; i++) {
        if (inode->blocks[i] != 0) {
            free_db_block(inode->blocks[i]);
            inode->blocks[i] = 0;
        }
    }
    free_inode(inode);
    free(path_copy);
    free(search);
    return 0;
}

int wfs_rmdir(const char *path) {
    wfs_unlink(path);
    return 0;
}

char *find_offset(struct wfs_inode *inode, off_t offset, int flag) {
    int index = offset / BLOCK_SIZE;
    off_t *arr;
    if (index > D_BLOCK) {
        index = index - IND_BLOCK;
        if (inode->blocks[IND_BLOCK] == 0) {
            if ((inode->blocks[IND_BLOCK] = allocate_DB()) < 0) {
                return NULL;
            }
        }
        arr = (off_t *)mmap_ptr(inode->blocks[IND_BLOCK]);
    }
    else {
        arr = inode->blocks;
    }
    if (*(arr + index) == 0 && flag) {   
        off_t block = allocate_DB();
        if(block < 0 || (*(arr + index) = block) == 0) {
            err = -ENOSPC;
            return NULL;
        }
    }
    return (char *)mmap_ptr(*(arr + index)) + (offset % BLOCK_SIZE);
}


char* mmap_ptr(off_t offset) { 
    return (char *)mapped_memory_region + offset;
}

void free_bitmap(uint32_t pos, uint32_t *bitmap) {
    bitmap[pos / 32] -= 1 << (pos % 32);
}

void free_db_block(off_t block) {
    memset(mmap_ptr(block), 0, BLOCK_SIZE);
    free_bitmap((block - ((struct wfs_sb *)mapped_memory_region)->d_blocks_ptr) / BLOCK_SIZE, (uint32_t *)mmap_ptr(((struct wfs_sb *)mapped_memory_region)->d_bitmap_ptr));
}

void free_inode(struct wfs_inode *inode) {
    memset(inode, 0, sizeof(struct wfs_inode));
    free_bitmap(((char*)inode - (char*)mmap_ptr(((struct wfs_sb *)mapped_memory_region)->i_blocks_ptr)) / BLOCK_SIZE, (uint32_t *)mmap_ptr(((struct wfs_sb *)mapped_memory_region)->i_bitmap_ptr));
}

struct wfs_inode *find_inode(int num) {
    uint32_t *bitmap = (uint32_t *)mmap_ptr(((struct wfs_sb *)mapped_memory_region)->i_bitmap_ptr);
    return (bitmap[num / 32] & (0x1 << (num % 32)))  ? (struct wfs_inode *)((char *)mmap_ptr(((struct wfs_sb *)mapped_memory_region)->i_blocks_ptr) + num * BLOCK_SIZE) : NULL;
}

size_t allocate_block(uint32_t *bitmap, size_t size) {
    for (uint32_t i = 0; i < size; i++) {
        uint32_t bitmap_region = bitmap[i];
        uint32_t k = 0;
        while (bitmap_region != UINT32_MAX && k < 32) {
            if (!((bitmap_region >> k) & 0x1)) {
                bitmap[i] = bitmap[i] | (0x1 << k);
                return i * 32 + k;
            }
            k++;
        }
    }
    return -1;
}

off_t allocate_DB() {
    struct wfs_sb *temp = (struct wfs_sb *)mapped_memory_region;
    off_t num = allocate_block((uint32_t *)mmap_ptr(temp->d_bitmap_ptr), temp->num_data_blocks / 32);
    if (num < 0) {
        err = -ENOSPC;  
        return -1;
    }
    return temp->d_blocks_ptr + BLOCK_SIZE * num;
}

struct wfs_inode *allocate_inode() {
    struct wfs_sb *temp = (struct wfs_sb *)mapped_memory_region;
    off_t num = allocate_block((uint32_t *)mmap_ptr(temp->i_bitmap_ptr), temp->num_inodes / 32);
    if (num < 0) {
        err = -ENOSPC;
        return NULL;
    }
    struct wfs_inode *inode = (struct wfs_inode *)((char *)mmap_ptr(temp->i_blocks_ptr) + num * BLOCK_SIZE);
    inode->num = num;
    return inode;
}

int get_inode(struct wfs_inode *enclosing, char *path, struct wfs_inode **inode) {
    if (!strcmp(path, "")) {
        *inode = enclosing;
        return 0;
    }
    char *path_temp = path;
    while (*path != '/' && *path != '\0') {
        path++;
    }
    if (*path != '\0') {
        *path = '\0';
        path++;
    }
    size_t size = enclosing->size;
    struct wfs_dentry *entry;
    int num = -1;
    for (off_t off = 0; off < size; off += sizeof(struct wfs_dentry)) {
        entry = (struct wfs_dentry *)find_offset(enclosing, off, 0);
        if(entry == NULL) {
            err = -ENOENT;
            return -1;
        }
        if (entry->num != 0 && !strcmp(entry->name, path_temp)) {
            num = entry->num;
            break;
        }
    }
    if (num < 0) {
        err = -ENOENT;
        return -1;
    }
    return get_inode(find_inode(num), path, inode);
}

int get_inode_path(char *path, struct wfs_inode **inode) {
    return get_inode(find_inode(0), path + 1, inode);
}

int main(int argc, char *argv[]) {
    int status;
    struct stat temp;
    int fd;
    char *disk_img = argv[1];
    for (int i = 2; i < argc; i++) {
        argv[i - 1] = argv[i];
    }
    argc = argc - 1;
    if ((fd = open(disk_img, O_RDWR)) < 0) {
        perror("Error opening file");
        exit(1);
    }
    if (fstat(fd, &temp) < 0) {
        perror("Error getting file stats");
        exit(1);
    }
    mapped_memory_region = mmap(NULL, temp.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_memory_region == MAP_FAILED) {
        perror("Error mapping memory");
        exit(1);
    }
    assert(find_inode(0) != NULL);
    status = fuse_main(argc, argv, &ops, NULL);
    munmap(mapped_memory_region, temp.st_size);
    close(fd);
    return status;
}
