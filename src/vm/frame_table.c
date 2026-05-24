#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "devices/timer.h"
#include "threads/thread.h"
#include "threads/malloc.h"   
#include "threads/palloc.h"   
#include "userprog/pagedir.h" 
#include "vm/frame_table.h"
#include "vm/swap.h"
#include <string.h>
#include "devices/timer.h"
#include "threads/vaddr.h"

struct list frame_table;
struct lock frame_table_lock;

static void *frame_evict (void);

void *
frame_alloc (enum palloc_flags flags)
{
    /* 1. Tenta pegar uma página livre na marra */
    void *kpage = palloc_get_page (flags);

    /* 2. Se a RAM lotou, precisamos expulsar alguém */
    if (kpage == NULL) 
    {
        /* O frame_evict deve: 
           - Escolher um frame 
           - pagedir_clear_page no dono antigo
           - Salvar no Swap/Disco se necessário
           - REMOVER o fte antigo da lista e dar free nele
           - Retornar o ponteiro kpage */
        kpage = frame_evict ();
        
        if (kpage == NULL) return NULL;

        /* Se o PAL_ZERO foi pedido, garantimos que a página expulsa seja zerada */
        if (flags & PAL_ZERO)
            memset (kpage, 0, PGSIZE);
    }

    /* 3. Criar a nova entrada para o NOVO dono do frame */
    struct frame_table_entry *fte = malloc (sizeof (struct frame_table_entry));
    if (fte == NULL) 
    {
        /* Se falhar o malloc, devolvemos a página pro sistema para não vazar memória */
        palloc_free_page (kpage);
        return NULL;
    }

    fte->frame = kpage;
    fte->owner = thread_current ();
    fte->spte = NULL; // Será preenchido logo após o retorno desta função
    fte->timestamp = timer_ticks ();

    /* 4. Registrar na tabela global com proteção de lock */
    lock_acquire (&frame_table_lock);
    list_push_back (&frame_table, &fte->elem);
    lock_release (&frame_table_lock);

    return kpage;
}
bool 
install_frame(void *upage, void *kpage, bool writable) 
{
    struct thread *t = thread_current();
    bool success = pagedir_set_page(t->pagedir, upage, kpage, writable);
    
    if (success) 
    {
        lock_acquire(&frame_table_lock);
        
        /* 1. Criar a entrada da Tabela Suplementar (SPT) para esta página */
        struct sup_page_table_entry *spte = malloc(sizeof(struct sup_page_table_entry));
        if (spte == NULL) {
            lock_release(&frame_table_lock);
            return false;
        }
        
        spte->user_vaddr = upage;
        spte->type = PAGE_ZERO; /* Por padrão, inicializamos como zero/stack growth */
        spte->is_loaded = true;
        spte->file = NULL;
        
        /* Insere na lista de páginas suplementares da THREAD */
        list_push_back(&t->sup_page_table, &spte->elem);

        /* 2. Encontrar o frame que acabamos de alocar e fazer o vínculo bidirecional */
        struct list_elem *e;
        for (e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e))
        {
            struct frame_table_entry *fte = list_entry(e, struct frame_table_entry, elem);
            if (fte->frame == kpage && fte->owner == t)
            {
                fte->spte = spte; /* O frame agora aponta para a SPT da thread */
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
        struct frame_table_entry *fte = list_entry(e, struct frame_table_entry, elem);

        if (fte->frame == kpage && fte->owner == thread_current())
        {
            // remove da lista 
            list_remove(&fte->elem);
            // libera a página física 
            palloc_free_page(kpage);
            // libera as structs
            if(fte->spte != NULL){
                list_remove(&fte->spte->elem);
                free(fte->spte);
            }
            free(fte);
            break;
        }
    }
    lock_release(&frame_table_lock);
}

/* Nova função auxiliar crucial para o page_fault encontrar páginas */
struct sup_page_table_entry *
spt_lookup(struct list *spt, void *user_vaddr) 
{
    struct list_elem *e;
    for (e = list_begin(spt); e != list_end(spt); e = list_next(e)) {
        struct sup_page_table_entry *spte = list_entry(e, struct sup_page_table_entry, elem);
        if (spte->user_vaddr == user_vaddr) {
            return spte;
        }
    }
    return NULL;
}

bool
load_page_from_file (struct sup_page_table_entry *spte)
{
    uint8_t *kpage = frame_alloc (PAL_USER);
    if (kpage == NULL) return false;

    if (spte->read_bytes > 0) 
    {
        extern struct lock lock_file;
        
        /* Uso correto e nativo do PintOS para Smart Locking */
        bool locked_by_me = lock_held_by_current_thread(&lock_file);
        
        if (!locked_by_me) lock_acquire(&lock_file);
        
        file_seek (spte->file, spte->file_offset);
        if (file_read (spte->file, kpage, spte->read_bytes) != (int) spte->read_bytes) 
        {
            if (!locked_by_me) lock_release(&lock_file);
            frame_free (kpage);
            return false;
        }
        
        if (!locked_by_me) lock_release(&lock_file);
    }
    
    memset (kpage + spte->read_bytes, 0, PGSIZE - spte->read_bytes);

    /* 3. Instalar o frame mapeando no diretório de páginas do processo */
    struct thread *t = thread_current ();
    if (!pagedir_set_page (t->pagedir, spte->user_vaddr, kpage, spte->writable)) 
    {
        frame_free (kpage);
        return false;
    }

    /* 4. Vincular o frame físico à nossa estrutura de controle global */
    lock_acquire (&frame_table_lock);
    struct list_elem *e;
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame_table_entry *fte = list_entry (e, struct frame_table_entry, elem);
        if (fte->frame == kpage && fte->owner == t)
        {
            fte->spte = spte;
            break;
        }
    }
    lock_release (&frame_table_lock);

    /* 5. Atualizar o estado na SPT */
    spte->is_loaded = true;

    return true;
}

void 
spt_destroy (struct list *spt) 
{
    /* Varre a lista liberando estritamente a memória dos nós da SPT */
    while (!list_empty (spt)) 
    {
        struct list_elem *e = list_pop_front (spt);
        struct sup_page_table_entry *spte = list_entry (e, struct sup_page_table_entry, elem);

        /* Se a página estava associada a um frame físico, precisamos apenas 
           remover o nó de controle da tabela global, SEM dar palloc_free_page aqui! */
        if (spte->is_loaded) 
        {
            lock_acquire (&frame_table_lock);
            struct list_elem *fe;
            for (fe = list_begin (&frame_table); fe != list_end (&frame_table); fe = list_next (fe)) 
            {
                struct frame_table_entry *fte = list_entry (fe, struct frame_table_entry, elem);
                if (fte->spte == spte) 
                {
                    list_remove (&fte->elem);
                    /* Removemos a estrutura de controle fte da lista global, 
                       mas DEIXAMOS o palloc_free_page por conta do pagedir_destroy! */
                    free (fte);
                    break;
                }
            }
            lock_release (&frame_table_lock);
        }
        else if (spte->type == PAGE_SWAP) 
        {
            swap_free (spte->swap_index); /* Libera o slot correspondente no bitmap */
        }

        /* Libera a memória do nó alocado no load_segment ou install_frame */
        free (spte);
    }
}
static void *
frame_evict (void)
{
  lock_acquire (&frame_table_lock);
  
  if (list_empty (&frame_table)) 
    {
      lock_release (&frame_table_lock);
      return NULL;
    }

  struct list_elem *e = list_begin (&frame_table);
  struct frame_table_entry *fte_to_evict = NULL;

  /* 1. SELEÇÃO: Usando o algoritmo do relógio (Clock) ou FIFO simples 
     para ser mais rápido que percorrer a lista inteira buscando ticks */
  fte_to_evict = list_entry (e, struct frame_table_entry, elem);

  /* 2. PREPARAÇÃO */
  struct sup_page_table_entry *spte = fte_to_evict->spte;
  void *kpage = fte_to_evict->frame;
  struct thread *owner = fte_to_evict->owner;

  /* Sincroniza o bit dirty do hardware com nossa estrutura antes de limpar */
  if (pagedir_is_dirty (owner->pagedir, spte->user_vaddr))
      spte->dirty = true;

  /* 3. LIMPEZA DE HARDWARE (Imediata) */
  pagedir_clear_page (owner->pagedir, spte->user_vaddr);
  spte->is_loaded = false;

  /* Remove da tabela global ANTES do I/O para evitar que outra thread 
     tente evictar o mesmo frame enquanto estamos fazendo swap */
  list_remove (&fte_to_evict->elem);
  lock_release (&frame_table_lock); // LIBERA O LOCK AQUI!

  /* 4. OPERAÇÃO DE I/O (Fora do lock da tabela de frames) */
  if (spte->type == PAGE_ZERO || spte->dirty)
    {
      /* Se for página de arquivo suja ou pilha, vai pro Swap */
      spte->swap_index = swap_out (kpage);
      spte->type = PAGE_SWAP;
    }
  
  /* Libera a estrutura de controle */
  free (fte_to_evict);

  return kpage; 
}
bool
load_page_from_swap (struct sup_page_table_entry *spte)
{
  /* 1. Conseguir um frame físico na RAM */
  uint8_t *kpage = frame_alloc (PAL_USER);
  if (kpage == NULL)
    return false;

  /* 2. Ler os dados do dispositivo de Swap de volta para a memória física */
  swap_in (spte->swap_index, kpage);

  /* 3. Instalar o frame mapeando no diretório de páginas de hardware do processo */
  struct thread *t = thread_current ();
  if (!pagedir_set_page (t->pagedir, spte->user_vaddr, kpage, spte->writable)) 
    {
      frame_free (kpage);
      return false;
    }

  /* 4. Vincular o frame físico na tabela global frame_table */
  lock_acquire (&frame_table_lock);
  struct list_elem *e;
  for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
      struct frame_table_entry *fte = list_entry (e, struct frame_table_entry, elem);
      if (fte->frame == kpage && fte->owner == t)
        {
          fte->spte = spte;
          break;
        }
    }
  lock_release (&frame_table_lock);

  /* 5. Atualizar os metadados da SPT */
  spte->is_loaded = true;
  
  /* IMPORTANTE: NÃO mude o type para PAGE_ZERO aqui! 
     Se a memória encher de novo, o evictor precisa saber que esta página 
     é do tipo SWAP para poder mandá-la de volta pro disco corretamente. */

  return true;
}