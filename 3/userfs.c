#include "userfs.h"
#include <string.h>
#include <stdlib.h>

#define FD_INIT_CAP 10
#define FD_GROW 2

enum { BLOCK_SIZE = 4096, MAX_FILE_SIZE = 104857600 };

static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    char *mem;
    int used;
    struct block *next, *prev;
};

struct file {
    struct block *first, *last;
    int refs;
    char *name;
    struct file *next, *prev;
    int deleted;
};

static struct file *all_files = NULL;

struct filedesc {
    struct file *file;
    int block_num, offset;
    enum open_flags flags;
};

static struct filedesc **fds = NULL;
static int fds_count = 0, fds_cap = 0;

enum ufs_error_code ufs_errno() { return ufs_error_code; }

static enum ufs_error_code init_fds() {
    fds = calloc(FD_INIT_CAP, sizeof(*fds));
    if (!fds) return UFS_ERR_NO_MEM;
    fds_cap = FD_INIT_CAP;
    return UFS_ERR_NO_ERR;
}

static enum ufs_error_code resize_fds() {
    int new_cap = fds_cap;
    if (fds_count == fds_cap) new_cap *= FD_GROW;
    else if (fds_count * FD_GROW < fds_cap && fds_cap > FD_INIT_CAP)
        new_cap /= FD_GROW;

    if (new_cap == fds_cap) return UFS_ERR_NO_ERR;
    struct filedesc **tmp = realloc(fds, sizeof(*fds) * new_cap);
    if (!tmp) return UFS_ERR_NO_MEM;
    memset(tmp + fds_count, 0, (new_cap - fds_count) * sizeof(*fds));
    fds = tmp;
    fds_cap = new_cap;
    return UFS_ERR_NO_ERR;
}

static enum ufs_error_code add_block(struct file *f) {
    struct block *b = calloc(1, sizeof(*b));
    if (!b) return UFS_ERR_NO_MEM;
    b->mem = calloc(BLOCK_SIZE, 1);
    if (!b->mem) { free(b); return UFS_ERR_NO_MEM; }
    if (!f->first)
        f->first = f->last = b;
    else {
        f->last->next = b;
        b->prev = f->last;
        f->last = b;
    }
    return UFS_ERR_NO_ERR;
}

static void free_blocks(struct block *b) {
    while (b) {
        struct block *next = b->next;
        free(b->mem);
        free(b);
        b = next;
    }
}

static struct file *create_file(const char *name) {
    struct file *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->name = strdup(name);
    if (!f->name) { free(f); return NULL; }
    if (add_block(f) != UFS_ERR_NO_ERR) { free(f->name); free(f); return NULL; }
    if (all_files) { f->next = all_files; all_files->prev = f; }
    all_files = f;
    return f;
}

static void remove_file(struct file *f) {
    if (f->prev) f->prev->next = f->next;
    if (f->next) f->next->prev = f->prev;
    if (f == all_files) all_files = f->next;
    free_blocks(f->first);
    free(f->name);
    free(f);
}

static struct file *find_file(const char *name) {
    for (struct file *f = all_files; f; f = f->next)
        if (!strcmp(f->name, name) && !f->deleted)
            return f;
    return NULL;
}

static struct filedesc *new_fd(struct file *f, enum open_flags flags) {
    struct filedesc *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->file = f;
    d->flags = flags;
    return d;
}

static int get_fd_slot() {
    if (!fds) return -1;
    for (int i = 0; i < fds_cap; i++)
        if (!fds[i]) return i;
    if (resize_fds() != UFS_ERR_NO_ERR) return -1;
    return fds_cap / FD_GROW;
}

static struct filedesc *fd_lookup(int fd) {
    if (fd < 0 || fd >= fds_count) return NULL;
    return fds[fd];
}

static int writable(struct filedesc *d) {
    return d->flags == 0 || (d->flags & (UFS_CREATE | UFS_WRITE_ONLY | UFS_READ_WRITE));
}

static int readable(struct filedesc *d) {
    return d->flags == 0 || (d->flags & (UFS_CREATE | UFS_READ_ONLY | UFS_READ_WRITE));
}

int ufs_open(const char *name, int flags) {
    if (!fds && init_fds() != UFS_ERR_NO_ERR) return -1;
    struct file *f = find_file(name);
    if (!f) {
        if (!(flags & UFS_CREATE)) { ufs_error_code = UFS_ERR_NO_FILE; return -1; }
        f = create_file(name);
        if (!f) { ufs_error_code = UFS_ERR_NO_MEM; return -1; }
    }
    int slot = get_fd_slot();
    if (slot == -1) return -1;
    struct filedesc *desc = new_fd(f, flags);
    if (!desc) { ufs_error_code = UFS_ERR_NO_MEM; return -1; }
    f->refs++;
    fds[slot] = desc;
    if (slot == fds_count) fds_count++;
    ufs_error_code = UFS_ERR_NO_ERR;
    return slot;
}

ssize_t ufs_write(int fd, const char *buf, size_t sz) {
    struct filedesc *desc = fd_lookup(fd);
    if (!desc) { ufs_error_code = UFS_ERR_NO_FILE; return -1; }
    if (!writable(desc)) { ufs_error_code = UFS_ERR_NO_PERMISSION; return -1; }
    struct file *f = desc->file;
    struct block *blk = f->first;
    for (int i = 0; i < desc->block_num; i++) blk = blk->next;
    size_t total = blk->used + desc->block_num * BLOCK_SIZE;
    if (total + sz > MAX_FILE_SIZE) { ufs_error_code = UFS_ERR_NO_MEM; return -1; }
    ssize_t written = 0;

    while (written < (ssize_t)sz) {
        if (desc->offset == BLOCK_SIZE) {
            blk = blk->next;
            if (!blk) {
                if (add_block(f) != UFS_ERR_NO_ERR) return written;
                blk = f->last;
            }
            desc->offset = 0;
            desc->block_num++;
        }
        size_t left = BLOCK_SIZE - desc->offset;
        if (sz - written < left) left = sz - written;
        memcpy(blk->mem + desc->offset, buf + written, left);
        desc->offset += left;
        written += left;
        if (desc->offset > blk->used) blk->used = desc->offset;
    }
    ufs_error_code = UFS_ERR_NO_ERR;
    return written;
}

ssize_t ufs_read(int fd, char *buf, size_t sz) {
    struct filedesc *desc = fd_lookup(fd);
    if (!desc) { ufs_error_code = UFS_ERR_NO_FILE; return -1; }
    if (!readable(desc)) { ufs_error_code = UFS_ERR_NO_PERMISSION; return -1; }
    struct block *blk = desc->file->first;
    for (int i = 0; i < desc->block_num; i++) blk = blk->next;
    ssize_t total_read = 0;

    while (total_read < (ssize_t)sz) {
        if (desc->offset == BLOCK_SIZE) {
            blk = blk->next;
            if (!blk) return total_read;
            desc->offset = 0;
            desc->block_num++;
        }
        size_t avail = blk->used - desc->offset;
        if (sz - total_read < avail) avail = sz - total_read;
        if (!avail) return total_read;
        memcpy(buf + total_read, blk->mem + desc->offset, avail);
        desc->offset += avail;
        total_read += avail;
    }
    return total_read;
}

int ufs_close(int fd) {
    struct filedesc *desc = fd_lookup(fd);
    if (!desc) { ufs_error_code = UFS_ERR_NO_FILE; return -1; }
    struct file *f = desc->file;
    f->refs--;
    if (f->deleted && f->refs == 0) remove_file(f);
    free(desc);
    fds[fd] = NULL;
    if (fd == fds_count - 1) while (fds_count > 0 && !fds[fds_count - 1]) fds_count--;
    resize_fds();
    return 0;
}

int ufs_delete(const char *name) {
    struct file *f = find_file(name);
    if (!f) { ufs_error_code = UFS_ERR_NO_FILE; return -1; }
    if (f->refs) f->deleted = 1;
    else remove_file(f);
    return 0;
}

int ufs_resize(int fd, size_t new_size) {
    struct filedesc *desc = fd_lookup(fd);
    if (!desc) { ufs_error_code = UFS_ERR_NO_FILE; return -1; }
    if (!writable(desc)) { ufs_error_code = UFS_ERR_NO_PERMISSION; return -1; }
    if (new_size > MAX_FILE_SIZE) { ufs_error_code = UFS_ERR_NO_MEM; return -1; }
    struct file *f = desc->file;
    struct block *blk = f->first;
    size_t size = 0;
    int blocks = 0;

    while (blk) {
        size += blk->used;
        if (size > new_size) break;
        blk = blk->next;
        blocks++;
    }

    if (size > new_size) {
        free_blocks(blk->next);
        f->last = blk;
        blk->used = new_size - blocks * BLOCK_SIZE;

        for (int i = 0; i < fds_count; i++) {
            struct filedesc *d = fds[i];
            if (!d || d->file != f) continue;
            if (d->block_num >= blocks) {
                d->block_num = blocks;
                if (d->offset > blk->used)
                    d->offset = blk->used;
            }
        }
    } else {
        if (blk) { size += BLOCK_SIZE - blk->used; blk->used = BLOCK_SIZE; }
        while (size < new_size) {
            if (add_block(f) != UFS_ERR_NO_ERR) return -1;
            f->last->used = BLOCK_SIZE;
            size += BLOCK_SIZE;
            blocks++;
        }
        f->last->used = new_size - blocks * BLOCK_SIZE;
    }
    return 0;
}

void ufs_destroy() {
    for (int i = 0; i < fds_count; i++) free(fds[i]);
    free(fds);
    while (all_files) remove_file(all_files);
}
