#include "filesys/inode.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include <debug.h>
#include <list.h>
#include <round.h>
#include <string.h>

#define BLOCK_SECTOR_SIZE 512
#define MAX_BLOCKS 12
#define N_DIRETOS 10
#define N_INDIRETOS 1
#define N_DUP_INDIRETOS 1


/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t setores[MAX_BLOCKS]; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  uint32_t unused[114]; /* Not used. */
};

// mostra um erro na compilação se o inode_disk tiver um tamanho diferente de
// 512 bytes
_Static_assert(sizeof(struct inode_disk) == 512,
               "inode_disk must be 512 bytes");

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) {
  return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos >= inode->data.length)
    return -1;
  block_sector_t sector_idx = pos / BLOCK_SECTOR_SIZE;
  if(sector_idx < N_DIRETOS) {
    return inode->data.setores[sector_idx];
  } 
  else if(sector_idx < 138) {
    block_sector_t setor_indireto = inode->data.setores[N_DIRETOS];
    if (setor_indireto == 0)
      return -1; 

    block_sector_t buffer[128];
    block_read (fs_device, setor_indireto, buffer);
    return buffer[sector_idx - 10];
  }
  else if(sector_idx < 16522) {
    block_sector_t setor_dup_indireto = inode->data.setores[N_DIRETOS + N_INDIRETOS];
    if(setor_dup_indireto == 0) 
      return -1;

    block_sector_t block_simples[128];
    block_sector_t double_idx = (sector_idx -  138) / 128;
    block_sector_t inside_idx = (sector_idx - 138) % 128;

    block_read (fs_device, setor_dup_indireto, block_simples);

    block_sector_t setor_indireto = block_simples[double_idx];
    block_sector_t block_indireto[128];
    block_read(fs_device, setor_indireto, block_indireto);
    return block_indireto[inside_idx];
  }
  return -1;
}

static bool inode_reserve (struct inode_disk *disk_inode, off_t target_length) {

  if (target_length <= disk_inode->length)
    return true; // Ja possui tamanho suficiente

  size_t sectors_needed = bytes_to_sectors (target_length);
  size_t i;
  static char zeros[BLOCK_SECTOR_SIZE];
  for (i = 0; i < sectors_needed; i++) {
    block_sector_t novo_setor;
    
    if (i < 10) {
    if (disk_inode->setores[i] == 0) {
      if (!free_map_allocate (1, &novo_setor))
        return false; // Disco cheio

      disk_inode->setores[i] = novo_setor;
      block_write (fs_device, novo_setor, zeros); // Inicializa com zeros
    }
  } else if(i < 138) {
    block_sector_t tabela_indireta[128];
    size_t idx_indireto = i - N_DIRETOS;


    // Cria a tabela de blocos indiretos se ainda nao existe
    if (disk_inode->setores[10] == 0) {
      if (!free_map_allocate (1, &novo_setor))
        return false;

      disk_inode->setores[10] = novo_setor;
      memset (tabela_indireta, 0, BLOCK_SECTOR_SIZE);
      block_write (fs_device, disk_inode->setores[10], tabela_indireta);
    } else {
      // Se ja existe, la a tabela atual do disco para a memoria(ram)
      block_read (fs_device, disk_inode->setores[10], tabela_indireta);
    }
    if (tabela_indireta[idx_indireto] == 0) {
      if (!free_map_allocate (1, &novo_setor))
        return false;

      tabela_indireta[idx_indireto] = novo_setor;
      // Grava o bloco de dados limpo
      block_write (fs_device, novo_setor, zeros); 
      // Atualiza a tabela de ponteiros modificada de volta no disco
      block_write (fs_device, disk_inode->setores[10], tabela_indireta);
    }
  }
  else {
    block_sector_t tabela_dupla_1[128];
    block_sector_t tabela_dupla_2[128];
    // Mapeia indices em relação ao limite do duplo indireto
    size_t idx_relativo = i - 138;
    size_t idx_tabela_1 = idx_relativo / 128; // Qual tabela de 2º nível
    size_t idx_tabela_2 = idx_relativo % 128; // Qual posição dentro dela

    // Gerenciar a tabela de nivel 1
    if (disk_inode->setores[11] == 0) {
      if (!free_map_allocate (1, &novo_setor)) return false;
      disk_inode->setores[11] = novo_setor;
      memset (tabela_dupla_1, 0, BLOCK_SECTOR_SIZE);
      block_write (fs_device, disk_inode->setores[11], tabela_dupla_1);
    } else {
      block_read (fs_device, disk_inode->setores[11], tabela_dupla_1);
    }
    // Gerenciar a tabela de nivel 2
    if (tabela_dupla_1[idx_tabela_1] == 0) {
      if (!free_map_allocate (1, &novo_setor)) return false;
      tabela_dupla_1[idx_tabela_1] = novo_setor;
      memset (tabela_dupla_2, 0, BLOCK_SECTOR_SIZE);
      block_write (fs_device, tabela_dupla_1[idx_tabela_1], tabela_dupla_2);
      // Atualiza a tabela pai (Nível 1) no disco
      block_write (fs_device, disk_inode->setores[11], tabela_dupla_1);
    } else {
      block_read (fs_device, tabela_dupla_1[idx_tabela_1], tabela_dupla_2);
    }

    // Alocar o bloco de dados real no disco
    if (tabela_dupla_2[idx_tabela_2] == 0) {
      if (!free_map_allocate (1, &novo_setor)) return false;
      tabela_dupla_2[idx_tabela_2] = novo_setor;
      block_write (fs_device, novo_setor, zeros);
      // Atualiza a tabela filha (nivel 2) no disco
      block_write (fs_device, tabela_dupla_1[idx_tabela_1], tabela_dupla_2);
    }
  }
  }
  disk_inode->length = target_length;
  return true;
}

static void inode_free_blocks(struct inode_disk *disk_inode)
{
    size_t sectors = bytes_to_sectors(disk_inode->length);
    size_t i;
    // blocos diretos
    for (i = 0; i < sectors && i < N_DIRETOS; i++)
        free_map_release(disk_inode->setores[i], 1);

    if (sectors <= N_DIRETOS)
        return;

    // Blocos indiretos
    size_t indirect_sectors = sectors - N_DIRETOS;
    if (indirect_sectors > 128)
        indirect_sectors = 128;

    if (disk_inode->setores[N_DIRETOS] != 0)
    {
        block_sector_t tabela_indireta[128];
        block_read(fs_device, disk_inode->setores[N_DIRETOS], tabela_indireta);

        for (i = 0; i < indirect_sectors; i++)
            if (tabela_indireta[i] != 0)
                free_map_release(tabela_indireta[i], 1);

        // libera a própria tabela indireta 
        free_map_release(disk_inode->setores[N_DIRETOS], 1);
    }

    if (sectors <= N_DIRETOS + 128)
        return;  

    size_t double_sectors = sectors - N_DIRETOS - 128;

    if (disk_inode->setores[N_DIRETOS + N_INDIRETOS] != 0)
    {
        block_sector_t tabela_dupla[128];
        block_read(fs_device, disk_inode->setores[N_DIRETOS + N_INDIRETOS], tabela_dupla);

        size_t num_tabelas = double_sectors / 128;

        for (size_t t = 0; t < num_tabelas+1; t++)
        {
            if (tabela_dupla[t] == 0)
                continue;

            block_sector_t tabela_filha[128];
            block_read(fs_device, tabela_dupla[t], tabela_filha);

            size_t restantes = double_sectors - t * 128;
            size_t a_liberar = restantes < 128 ? restantes : 128;

            for (i = 0; i < a_liberar; i++)
                if (tabela_filha[i] != 0)
                    free_map_release(tabela_filha[i], 1);

            // libera a tabela de nivel 2
            free_map_release(tabela_dupla[t], 1);
        }

        // libera a tabela de nivel 1(duplo indireto)
        free_map_release(disk_inode->setores[N_DIRETOS + N_INDIRETOS], 1);
    }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create(block_sector_t sector, off_t length)
{
    struct inode_disk *disk_inode = NULL;
    bool success = false;

    ASSERT(length >= 0);
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode != NULL)
    {
        disk_inode->length = 0;       
        disk_inode->magic  = INODE_MAGIC;

        if (inode_reserve(disk_inode, length))
        {
            block_write(fs_device, sector, disk_inode);
            success = true;
        }

        free(disk_inode);
    }
    return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open(block_sector_t sector) {
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read(fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen(struct inode *inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode *inode) {
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      inode_free_blocks(&inode->data); 
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size,
                    off_t offset) {
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      block_read(fs_device, sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
         into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      block_read(fs_device, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset)
{
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;
    uint8_t *bounce = NULL;

    if (inode->deny_write_cnt)
        return 0;

    if (offset + size > inode_length(inode))
    {
        if (!inode_reserve(&inode->data, offset + size))
            return 0; 

        // persiste o novo length e os novos setores alocados 
        block_write(fs_device, inode->sector, &inode->data);
    }

    while (size > 0)
    {
        /* Sector to write, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector(inode, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
            block_write(fs_device, sector_idx, buffer + bytes_written);
        }
        else
        {
            if (bounce == NULL)
            {
                bounce = malloc(BLOCK_SECTOR_SIZE);
                if (bounce == NULL)
                    break;
            }

            if (sector_ofs > 0 || chunk_size < sector_left)
                block_read(fs_device, sector_idx, bounce);
            else
                memset(bounce, 0, BLOCK_SECTOR_SIZE);

            memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
            block_write(fs_device, sector_idx, bounce);
        }

        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }
    free(bounce);

    return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) { return inode->data.length; }