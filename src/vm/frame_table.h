#ifndef FRAMET_FRAME_H
#define FRAMET_FRAME_H

#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <threads/palloc.h>
#include "threads/synch.h"
#include "threads/thread.h"

extern struct list frame_table;

extern struct lock frame_table_lock;

struct thread;

struct sup_page_table_entry {
    void* user_addr;
    uint64_t access_time;
    bool dirty;
    bool accessed;
    struct list_elem elem;
};

struct frame_table_entry {
    void *frame;
    struct thread *owner;
    struct sup_page_table_entry *aux;
    struct list_elem elem;
};


void *frame_alloc(enum palloc_flags flags);
bool install_frame(void *upage, void *kpage, bool writable);
void frame_free(void *frame);

#endif