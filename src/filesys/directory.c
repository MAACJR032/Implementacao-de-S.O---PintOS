#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry),true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (strcmp (name, ".") == 0)
    {
      *inode = inode_reopen (dir_get_inode (dir));
      return true;
    }

  /* Se o nome for ".." e estivermos no diretório raiz,
     retornamos um novo ponteiro para o próprio inode da raiz.
     No Pintos, o diretório raiz fica fixo no setor ROOT_DIR_SECTOR (geralmente setor 1). */
  if (strcmp (name, "..") == 0 && inode_get_inumber (dir_get_inode (dir)) == ROOT_DIR_SECTOR) 
    {
      *inode = inode_reopen (dir_get_inode (dir));
      return true;
    }

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

/* Analisa um PATH absoluto ou relativo.
   Retorna o 'struct dir *' correspondente ao diretório pai do alvo
   e copia o nome do arquivo/diretório final para 'file_name'.
   Retorna NULL em caso de falha. */
struct dir *
path_resolve (const char *path, char *file_name)
{
  if (path == NULL || file_name == NULL || strlen(path) == 0)
    return NULL;

  struct dir *dir_atual;

  /* 1. Determina o ponto de partida (Absoluto vs Relativo) */
  if (path[0] == '/') 
    {
      dir_atual = dir_open_root (); /* Começa na Raiz */
    } 
  else 
    {
      if (thread_current()->cwd != NULL)
        dir_atual = dir_reopen (thread_current()->cwd); /* Começa no CWD */
      else
        dir_atual = dir_open_root ();
    }

  /* Se o ponto de partida foi removido, a resolução deve falhar imediatamente */
  if (inode_is_removed (dir_get_inode (dir_atual))) 
    {
      dir_close (dir_atual);
      return NULL;
    }

  /* Fazemos uma cópia do path original porque strtok_r modifica a string */
  char *path_copy = malloc (strlen (path) + 1);
  if (path_copy == NULL) 
    {
      dir_close (dir_atual);
      return NULL;
    }
  strlcpy (path_copy, path, strlen (path) + 1);

  char *token, *save_ptr;
  char *next_token = strtok_r (path_copy, "/", &save_ptr);
  token = next_token;

  /* Se o caminho era apenas "/", tratamos como diretório raiz com nome vazio */
  if (token == NULL) 
    {
      file_name[0] = '\0';
      free (path_copy);
      return dir_atual;
    }

  /* Avança para obter o próximo token adiantado */
  next_token = strtok_r (NULL, "/", &save_ptr);

  /* Loop de navegação pelos subdiretórios intermédios */
  while (next_token != NULL) 
    {
      if (strlen (token) > NAME_MAX) 
        {
          dir_close (dir_atual);
          free (path_copy);
          return NULL;
        }

      struct inode *next_inode = NULL;

      /* Tratamento dos componentes especiais '.' e '..' */
      if (strcmp (token, ".") == 0) 
        {
          /* Mantém-se no mesmo diretório */
          next_inode = inode_reopen (dir_get_inode (dir_atual));
        } 
      else if (strcmp (token, "..") == 0) 
        {
          /* Busca pelo Inode Pai. Nota: Vocês precisarão garantir que o dir_lookup
             ou o próprio inode de um diretório guarde uma entrada especial ".." */
          if (!dir_lookup (dir_atual, "..", &next_inode)) 
            {
              dir_close (dir_atual);
              free (path_copy);
              return NULL; /* Falha ao subir nível */
            }
        } 
      else 
        {
          /* Procura normal por subdiretório */
          if (!dir_lookup (dir_atual, token, &next_inode)) 
            {
              dir_close (dir_atual);
              free (path_copy);
              return NULL; /* Subdiretório não encontrado */
            }
        }

      /*Garante que o inode encontrado é realmente de um diretório e não foi removido */
      if (!inode_is_dir (next_inode) || inode_is_removed (next_inode)) 
        {
          inode_close (next_inode);
          dir_close (dir_atual);
          free (path_copy);
          return NULL; /* Corta a operação se tentar usar arquivo como se fosse pasta */
        }

      dir_close (dir_atual);
      dir_atual = dir_open (next_inode);
      
      token = next_token;
      next_token = strtok_r (NULL, "/", &save_ptr);
    }

  /* O último token restante é o nome do arquivo/diretório final */
  if (strlen (token) > NAME_MAX) 
    {
      dir_close (dir_atual);
      free (path_copy);
      return NULL;
    }
  strlcpy (file_name, token, NAME_MAX + 1);

  free (path_copy);
  return dir_atual;
}

/* Retorna true se o diretório estiver vazio (apenas contendo "." e ".."), 
   ou false caso contrário. */
bool
dir_is_empty (struct dir *dir) 
{
  char name[NAME_MAX + 1];
  block_sector_t inode_sector;
  
  /* Reinicia o ponteiro de leitura do diretório */
  dir_seek (dir, 0); 
  
  /* Varre todas as entradas internas */
  while (dir_readdir (dir, name)) 
    {
      /* Se encontrar qualquer entrada que não seja "." ou "..", o diretório não está vazio */
      if (strcmp (name, ".") != 0 && strcmp (name, "..") != 0) 
        {
          return false;
        }
    }
  return true;
}

/* Define a posição de leitura atual no diretório para POS. */
void
dir_seek (struct dir *dir, off_t pos) 
{
  ASSERT (dir != NULL);
  ASSERT (pos >= 0);
  dir->pos = pos;
}

/* Retorna a posição de leitura atual no diretório. */
off_t
dir_tell (struct dir *dir) 
{
  ASSERT (dir != NULL);
  return dir->pos;
}