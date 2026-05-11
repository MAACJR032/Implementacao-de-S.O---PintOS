#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "devices/timer.h"
#include "threads/thread.h"
#include "threads/malloc.h"   
#include "threads/palloc.h"   
#include "userprog/pagedir.h" 
#include "frame_table.h"

struct list frame_table;


struct lock frame_table_lock;
void *
frame_alloc(enum palloc_flags flags)
{
    lock_acquire(&frame_table_lock);
    // chama palloc internamente 
    uint32_t *kpage = palloc_get_page(flags);

    if (kpage != NULL)
    {
        /* cria e registra a entrada */
        struct frame_table_entry *fte = malloc(sizeof(struct frame_table_entry));
        if (fte == NULL) {
            palloc_free_page(kpage);
            lock_release(&frame_table_lock);
            return NULL;
        }
        fte->frame = kpage;
        fte->owner = thread_current();
        fte->aux = NULL;
        list_push_back(&frame_table, &fte->elem);
    }
    lock_release(&frame_table_lock);
    return kpage;
}

bool install_frame(void *upage, void *kpage, bool writable) {
    bool success = false;
    struct list_elem *e;
    struct thread *t = thread_current();
    success = (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
    if(success) {
        lock_acquire(&frame_table_lock);
        for (e = list_begin(&frame_table);
            e != list_end(&frame_table);
            e = list_next(e))
        {
            struct frame_table_entry *fte = list_entry(e, struct frame_table_entry, elem);
            if (fte->frame == kpage && fte->owner == thread_current())
            {
                fte->aux = malloc(sizeof(struct sup_page_table_entry));
                if(fte->aux == NULL) {
                    lock_release(&frame_table_lock);
                    return false;
                }
                fte->aux->user_addr = upage;
                fte->aux->access_time = timer_ticks();
                fte->aux->dirty = false;
                fte->aux->accessed = false;
                break;
            }

        }
        lock_release(&frame_table_lock);
    } 
    return success;
    
}
void
frame_free(void *kpage)
{
    lock_acquire(&frame_table_lock);

    struct list_elem *e;
    for (e = list_begin(&frame_table);
         e != list_end(&frame_table);
         e = list_next(e))
    {
        struct frame_table_entry *fte =
            list_entry(e, struct frame_table_entry, elem);

        if (fte->frame == kpage && fte->owner == thread_current())
        {
            // remove da lista 
            list_remove(&fte->elem);
            // libera a página física 
            palloc_free_page(kpage);
            // libera as structs
            if(fte->aux != NULL)
                free(fte->aux);
            free(fte);
            break;
        }
    }
    lock_release(&frame_table_lock);
}

