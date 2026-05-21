#ifndef FRAMET_FRAME_H
#define FRAMET_FRAME_H

#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <threads/palloc.h>
#include "threads/synch.h"
#include "filesys/file.h"

extern struct list frame_table;
extern struct lock frame_table_lock;

enum page_type {
    PAGE_ZERO,   /* Páginas zeradas (ex: Stack Growth) */
    PAGE_FILE,   /* Páginas do executável/arquivo em disco */
    PAGE_SWAP    /* Páginas que foram para o Swap */
};

struct sup_page_table_entry {
    void* user_vaddr;
    enum page_type type;          /* Identifica a origem da página */
    bool is_loaded;               /* Indica se está na memória RAM ou não */

    /* Dados para controle de arquivos (Demand Paging) */
    struct file *file;
    off_t file_offset;
    uint32_t read_bytes;
    bool writable;

    /* Dados para controle de Swap (Evicção) [cite: 219, 438] */
    size_t swap_index;            /* Slot reservado no disco de swap [cite: 438] */
    struct list_elem elem;
};

struct frame_table_entry {
    void *frame;
    struct thread *owner;
    struct sup_page_table_entry *spte;
    int64_t timestamp;                  /* ADICIONADO: Registro do timer_ticks() */
    struct list_elem elem;
};

/* Funções globais do módulo */
void *frame_alloc(enum palloc_flags flags);
bool install_frame(void *upage, void *kpage, bool writable);
void frame_free(void *frame);

/* Funções da SPT que vocês vão usar no page_fault */
struct sup_page_table_entry *spt_lookup(struct list *spt, void *user_vaddr);
void spt_destroy (struct list *spt);
bool load_page_from_file (struct sup_page_table_entry *spte);
bool load_page_from_swap (struct sup_page_table_entry *spte);
#endif