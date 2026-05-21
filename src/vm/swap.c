#include "vm/swap.h"
#include "threads/vaddr.h"
#include <bitmap.h>

static struct block *swap_device;
static struct bitmap *swap_table;
static struct lock swap_lock;

/* Inicializa o dispositivo de swap e o bitmap de controle */
void
swap_init (void)
{
    /* Obtém o bloco inicializado para operações de Swap */
    swap_device = block_get_role (BLOCK_SWAP); // [cite: 392, 399]
    if (swap_device == NULL) {
        PANIC ("Não foi possível inicializar o dispositivo de Swap!");
    }

    /* Cada slot de swap precisa de 8 setores (4096 bytes / 512 bytes) */
    size_t swap_slots = block_size (swap_device) / 8;

    /* Cria um bitmap onde cada bit representa um slot de swap (false = livre, true = ocupado) */
    swap_table = bitmap_create (swap_slots);
    if (swap_table == NULL) {
        PANIC ("Não foi possível alocar o bitmap do Swap!");
    }

    lock_init (&swap_lock);
}

/* Escreve uma página da RAM para o Swap e retorna o índice do slot ocupado */
size_t
swap_out (void *frame)
{
    lock_acquire (&swap_lock);

    /* Encontra o primeiro slot livre (false) no bitmap e muda para ocupado (true) */
    size_t swap_index = bitmap_scan_and_flip (swap_table, 0, 1, false);
    if (swap_index == BITMAP_ERROR) {
        PANIC ("O Swap está completamente cheio! Out of swap space.");
    }

    /* Escreve os 4096 bytes do frame em 8 setores consecutivos do bloco */
    for (int i = 0; i < 8; i++) 
    {
        /* Calcula o setor exato no disco e o deslocamento no frame de 512 em 512 bytes */
        block_write (swap_device, swap_index * 8 + i, (uint8_t *) frame + (i * BLOCK_SECTOR_SIZE)); // [cite: 404, 407, 414, 415]
    }

    lock_release (&swap_lock);
    return swap_index;
}

/* Lê os dados do Swap de volta para um frame físico na RAM */
void
swap_in (size_t swap_index, void *frame)
{
    lock_acquire (&swap_lock);

    /* Verifica se o slot estava realmente ocupado antes de ler */
    ASSERT (bitmap_test (swap_table, swap_index) == true);

    /* Lê os 8 setores do disco jogando para a memória RAM */
    for (int i = 0; i < 8; i++) 
    {
        block_read (swap_device, swap_index * 8 + i, (uint8_t *) frame + (i * BLOCK_SECTOR_SIZE)); // [cite: 403, 407, 414, 415]
    }

    /* Marca o slot de volta como livre (false) */
    bitmap_flip (swap_table, swap_index);

    lock_release (&swap_lock);
}

/* Libera um slot do swap sem ler os dados (usado quando um processo morre) */
void
swap_free (size_t swap_index)
{
    lock_acquire (&swap_lock);
    ASSERT (bitmap_test (swap_table, swap_index) == true);
    bitmap_set (swap_table, swap_index, false);
    lock_release (&swap_lock);
}