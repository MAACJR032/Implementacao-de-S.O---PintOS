#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"

#define CACHE_SIZE 64 /* Limite máximo exigido pelo Pintos */

void cache_init (void);
void cache_read (struct block *block, block_sector_t sector, void *buffer);
void cache_write (struct block *block, block_sector_t sector, const void *buffer);
void cache_flush (struct block *block);

#endif /* filesys/cache.h */