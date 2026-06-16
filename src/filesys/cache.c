#include "filesys/cache.h"
#include "threads/synch.h"
#include <string.h>

struct cache_entry {
    block_sector_t sector;           /* Setor do disco associado */
    uint8_t data[BLOCK_SECTOR_SIZE]; /* Os 512 bytes de dados reais */
    bool valid;                      /* Indica se o bloco contém dados úteis */
    bool dirty;                      /* Modificado na RAM e pendente de gravação no disco */
    bool accessed;                   /* Bit usado pelo algoritmo de substituição Clock */
    struct lock entry_lock;          /* Lock para sincronização desta entrada individual */
};

static struct cache_entry cache[CACHE_SIZE];
static struct lock cache_global_lock; /* Lock para proteger a busca por blocos */
static size_t clock_hand;             /* Ponteiro/Mão do algoritmo Clock */

/* Inicializa as estruturas do cache */
void
cache_init (void)
{
  lock_init (&cache_global_lock);
  clock_hand = 0;
  
  for (size_t i = 0; i < CACHE_SIZE; i++)
    {
      cache[i].valid = false;
      cache[i].dirty = false;
      cache[i].accessed = false;
      cache[i].sector = -1;
      lock_init (&cache[i].entry_lock);
    }
}

/* Escolhe uma entrada para evicção usando o algoritmo Clock.
   Assume que o cache_global_lock já foi adquirido. Retorna o índice da entrada livre. */
static size_t
cache_evict (struct block *block)
{
  while (true)
    {
      // Avança a mão do relógio circularmente
      struct cache_entry *entry = &cache[clock_hand];
      
      if (!entry->valid)
        {
          size_t free_idx = clock_hand;
          clock_hand = (clock_hand + 1) % CACHE_SIZE;
          return free_idx;
        }
        
      if (entry->accessed)
        {
          /* Segunda chance dada: limpa o bit de acesso e continua a busca */
          entry->accessed = false;
        }
      else
        {
          /* Encontrou um candidato para evicção */
          lock_acquire (&entry->entry_lock);
          
          if (entry->dirty)
            {
              /* Write-behind: Se foi modificado, grava de volta no disco físico */
              block_write (block, entry->sector, entry->data);
              entry->dirty = false;
            }
            
          entry->valid = false;
          lock_release (&entry->entry_lock);
          
          size_t evict_idx = clock_hand;
          clock_hand = (clock_hand + 1) % CACHE_SIZE;
          return evict_idx;
        }
        
      clock_hand = (clock_hand + 1) % CACHE_SIZE;
    }
}

void
cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  lock_acquire (&cache_global_lock);
  
  /* 1. Procura se o bloco já está no cache */
  for (size_t i = 0; i < CACHE_SIZE; i++)
    {
      if (cache[i].valid && cache[i].sector == sector)
        {
          lock_acquire (&cache[i].entry_lock);
          cache[i].accessed = true;
          lock_release (&cache_global_lock);
          
          memcpy (buffer, cache[i].data, BLOCK_SECTOR_SIZE);
          lock_release (&cache[i].entry_lock);
          return;
        }
    }
    
  /* 2. Cache Miss: Seleciona uma entrada (limpando/ejetando se necessário) */
  size_t idx = cache_evict (block);
  
  lock_acquire (&cache[idx].entry_lock);
  cache[idx].valid = true;
  cache[idx].sector = sector;
  cache[idx].dirty = false;
  cache[idx].accessed = true;
  
  lock_release (&cache_global_lock);
  
  /* Lê o dado do disco real para a estrutura do cache na RAM */
  block_read (block, sector, cache[idx].data);
  memcpy (buffer, cache[idx].data, BLOCK_SECTOR_SIZE);
  
  lock_release (&cache[idx].entry_lock);
}

void
cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  lock_acquire (&cache_global_lock);
  
  /* 1. Procura se o bloco já está no cache */
  for (size_t i = 0; i < CACHE_SIZE; i++)
    {
      if (cache[i].valid && cache[i].sector == sector)
        {
          lock_acquire (&cache[i].entry_lock);
          cache[i].accessed = true;
          cache[i].dirty = true;
          lock_release (&cache_global_lock);
          
          memcpy (cache[i].data, buffer, BLOCK_SECTOR_SIZE);
          lock_release (&cache[i].entry_lock);
          return;
        }
    }
    
  /* 2. Cache Miss na escrita */
  size_t idx = cache_evict (block);
  
  lock_acquire (&cache[idx].entry_lock);
  cache[idx].valid = true;
  cache[idx].sector = sector;
  cache[idx].dirty = true;
  cache[idx].accessed = true;
  
  lock_release (&cache_global_lock);
  
  /* Atualiza a RAM. Nota: Não escrevemos no disco agora (adiado) */
  memcpy (cache[idx].data, buffer, BLOCK_SECTOR_SIZE);
  
  lock_release (&cache[idx].entry_lock);
}

void
cache_flush (struct block *block)
{
  lock_acquire (&cache_global_lock);
  for (size_t i = 0; i < CACHE_SIZE; i++)
    {
      if (cache[i].valid && cache[i].dirty)
        {
          lock_acquire (&cache[i].entry_lock);
          block_write (block, cache[i].sector, cache[i].data);
          cache[i].dirty = false;
          lock_release (&cache[i].entry_lock);
        }
    }
  lock_release (&cache_global_lock);
}