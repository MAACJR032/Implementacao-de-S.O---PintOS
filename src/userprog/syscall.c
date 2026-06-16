#include "devices/shutdown.h"
#include "devices/input.h"
#include "threads/vaddr.h"
#include <string.h>
#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/pagedir.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/free-map.h"

struct lock lock_file;//adicionei
#define VAR1 (*(uint32_t *)(f->esp + 4)) //Variavel "coringa" (ADICIONEI)
#define VAR2 (*(uint32_t *)(f->esp + 8)) //Variavel "coringa2" (ADICIONEI)

static bool
is_valid_ptr(const void *ptr)
{
    return ptr != NULL
        && is_user_vaddr(ptr)
        && pagedir_get_page(thread_current()->pagedir, ptr) != NULL;
}

static void
check_valid_string(const void *str)
{
  if (str == NULL) {
    exit(-1);
  }

  char *ptr = (char *)str;
  while (true) {
    // 1. PRIMEIRO verificamos se o endereço deste byte é válido na memória do usuário
    if (!is_valid_ptr(ptr)) {
      exit(-1); // Se cruzou para memória inválida antes do \0, mata o processo
    }
    
    // 2. SÓ DEPOIS de confirmar que é seguro, lemos o conteúdo para ver se a string acabou
    if (*ptr == '\0') {
      break;
    }
    
    // Avança para o próximo byte
    ptr++;
  }
}

static void
check_valid_buffer (const void *buffer, unsigned size)
{
  char *ptr = (char *) buffer;
  for (unsigned i = 0; i < size; i++) 
  {
    if (!is_valid_ptr(ptr + i)) 
    {
      exit(-1); // Se qualquer byte do buffer for inválido, mata o processo
    }
  }
}
static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  lock_init(&lock_file);//adicionei
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Implementação de exit para o kernel */
void exit(int status)
{
  struct thread *cur = thread_current();
  printf("%s: exit(%d)\n", cur->name, status);
  cur->exit_status = status;
  for(int i = 3;i<128;i++){
    // Fecha os arquivos abertos por aquele processo
    if(cur->DA[i]){
      lock_acquire(&lock_file); // ADICIONE O LOCK AQUI
      file_close(cur->DA[i]);
      lock_release(&lock_file); // LIBERE O LOCK AQUI
      cur->DA[i] = NULL;
    }
  }
  thread_exit();
}

/* Implementação mínima de write para o kernel */
int write(int fd, const void *buffer, unsigned size)
{
  if (fd == 1)
  {
    // stdout
    putbuf(buffer, size);
    return size;
  }
  return -1;
}

static void
syscall_handler (struct intr_frame *f) 
{
  if (f == NULL)
  {
    exit (-1);
  }
  // 1. Valida os 4 bytes do número da syscall
  check_valid_buffer(f->esp, sizeof(int));
  int syscall_num = *(int *) f->esp;
  switch (syscall_num)
  {

    //adicionei
    case SYS_HALT: 
    {
      shutdown_power_off();
      break;
    }

    //ajustei
    case SYS_EXIT:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      exit((int)VAR1);
      break;
    }
    
    //adicionei
    case SYS_EXEC:
    {
      check_valid_buffer(f->esp+4, sizeof(void*));

      const char *cmd_line = (const char*)VAR1;
      
      // Valida a string byte a byte até o \0
      check_valid_string(cmd_line);

      lock_acquire(&lock_file);      
      f->eax = process_execute((const char *)VAR1);
      lock_release(&lock_file);

      break;
    }
    
    //ajustei
    case SYS_WAIT:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      f->eax = process_wait((tid_t)VAR1);
      break;
    }

    //adicionei
    case SYS_CREATE:
    {
      if (!is_valid_ptr(f->esp + 4) || !is_valid_ptr(f->esp + 8))
        exit(-1);

      const char *file_name = (const char*)VAR1;

      // Valida a string inteira do nome do arquivo!
      check_valid_string(file_name);
      
      // Verifica se o ponteiro da string do nome do arquivo é válido
      if (!is_valid_ptr(file_name))
        exit(-1);

      lock_acquire(&lock_file); // ADICIONADO
      f->eax = filesys_create(file_name, (unsigned)VAR2);
      lock_release(&lock_file); // ADICIONADO
      break;
    }

    //adicionei
    case SYS_REMOVE:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      const char* file = (const char*)VAR1;
      check_valid_string(file);
      
      if (!is_valid_ptr(file))
        exit(-1);
      
      lock_acquire(&lock_file);
      
      bool success = false;
      
      /*Abre o arquivo/diretório temporariamente para inspecionar o Inode */
      struct file *target_file = filesys_open(file);
      if (target_file != NULL) 
        {
          struct inode *inode = file_get_inode(target_file);
          
          if (inode != NULL && inode_is_dir(inode)) 
            {
              /* REFINAMENTO: Se for um diretório */
              struct dir *dir = dir_open(inode_reopen(inode));
              
              /* Regra 1: Não pode remover o diretório raiz (setor ROOT_DIR_SECTOR)
                 Regra 2: O diretório precisa estar vazio */
              if (inode_get_inumber(inode) != ROOT_DIR_SECTOR && dir_is_empty(dir)) 
                {
                  file_close(target_file);
                  success = filesys_remove(file);
                }
              else 
                {
                  /* Falha se tentar apagar a raiz ou se a pasta tiver arquivos */
                  success = false; 
                  file_close(target_file);
                }
              dir_close(dir);
            }
          else 
            {
              /* Se for um arquivo comum, fecha o ponteiro temporário e remove normalmente */
              file_close(target_file);
              success = filesys_remove(file);
            }
        }
      else 
        {
          /* Arquivo nem sequer existia */
          success = false; 
        }

      f->eax = success;
      lock_release(&lock_file);
      break;
    }
    
    case SYS_OPEN:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      const char *file_name = (const char*)VAR1;

      // Valida a string inteira do nome do arquivo!
      check_valid_string(file_name);

      if (!is_valid_ptr(file_name))
        exit(-1);
      
      lock_acquire(&lock_file);

      struct file *file_open = filesys_open(file_name);
      int resultado = -1;

      if(file_open){
        struct inode *inode = file_get_inode(file_open);

        for(int i = 3;i<128;i++){
          if(!thread_current()->DA[i]){
            if (!strcmp(thread_current()->name, file_name) && 
                (inode != NULL && !inode_is_dir(inode))) {
              file_deny_write(file_open);
            }
            thread_current()->DA[i] = file_open;
            resultado=i;
            break;
          }
        }

        //caso não tenha slot em DA, fazer isso para evitar vazamento
        if(resultado == -1)
        file_close(file_open);
        
      }

      lock_release(&lock_file);
      f->eax = resultado;

      break;
    }

    // ADD
    case SYS_FILESIZE:
    {
      // 1. Valida o ponteiro da pilha para o argumento
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      // 2. Obtém o file descriptor através da sua macro
      int fd = (int)VAR1;

      // 3. Validação do FD: Garante que está nos limites do array (3 a 127) e que está aberto
      if (fd < 3 || fd >= 128 || thread_current()->DA[fd] == NULL) {
        exit(-1); 
      }

      // 4. Sincronização: Bloqueia o sistema de arquivos para leitura
      lock_acquire(&lock_file);

      // 5. Chamada ao File System e retorno via eax
      f->eax = file_length(thread_current()->DA[fd]);

      // 6. Liberação do lock
      lock_release(&lock_file);

      break;
    }

    // ADD
    case SYS_READ:
    {
      // 1. Valida se os endereços dos três argumentos na pilha são válidos
      if (!is_valid_ptr(f->esp + 4) || !is_valid_ptr(f->esp + 8) || !is_valid_ptr(f->esp + 12)) {
        exit(-1);
      }

      // 2. Extrai os argumentos da pilha
      int fd = (int)VAR1;
      void *buffer = (void *)VAR2;
      unsigned size = (unsigned)(*(uint32_t *)(f->esp + 12));

      // 3. Valida TODO o buffer de usuário (extremamente importante no read)
      check_valid_buffer(buffer, size);

      // 4. Lógica de Leitura
      if (fd == 0) {
        // Lê do teclado (stdin) usando input_getc()
        uint8_t *buf = (uint8_t *)buffer;
        for (unsigned i = 0; i < size; i++) {
          buf[i] = input_getc();
        }
        f->eax = size;
      } 
      else if (fd >= 3 && fd < 128) {
        // Obtém o arquivo da tabela de descritores do processo
        struct file *f_ptr = thread_current()->DA[fd];
        
        // Se o arquivo não estiver aberto, encerra o processo
        if (f_ptr == NULL) {
          exit(-1);
        }

        struct inode *inode = file_get_inode (f_ptr);
        if (inode != NULL && inode_is_dir (inode)) {
          f->eax = -1;
          break;
        }

        f->eax = file_read(f_ptr, buffer, size);
      } 
      else {
        // Tentar ler de stdout (fd == 1) ou fds inválidos resulta em erro
        f->eax = -1;
      }
      break;
    }
    
    // ADD
    case SYS_WRITE:
    {
        // 1. Valida se os endereços dos três argumentos na pilha são válidos
        if (!is_valid_ptr(f->esp + 4) || !is_valid_ptr(f->esp + 8) || !is_valid_ptr(f->esp + 12)) {
            exit(-1);
        }

        // 2. Extrai os argumentos usando a lógica existente
        int fd = (int)VAR1;
        const void *buffer = (const void *)VAR2;
        unsigned size = (unsigned)(*(uint32_t *)(f->esp + 12)); // Criando a lógica para um "VAR3"

        // 3. Valida TODO o buffer, e não apenas o primeiro byte
        check_valid_buffer(buffer, size);

        // 4. Lógica de Escrita
        if (fd == 1) {
            // Escreve no console
            putbuf(buffer, size);
            f->eax = size;
        } 
        else if (fd >= 3 && fd < 128) {
            // Escreve em um arquivo
            struct file *f_ptr = thread_current()->DA[fd];
            if (f_ptr == NULL) {
                exit(-1);
            }
            
            struct inode *inode = file_get_inode (f_ptr);
            if (inode != NULL && inode_is_dir (inode)) {
                f->eax = -1;
                break;
            }

            f->eax = file_write(f_ptr, buffer, size);
        } 
        else {
            // fd 0 (stdin) ou fds inválidos
            f->eax = -1; // ou exit(-1), dependendo da especificação do seu professor
        }
        break;
    }

    //adicionei
    case SYS_SEEK:
    {
      if (!is_valid_ptr(f->esp + 4) || !is_valid_ptr(f->esp + 8))
        exit(-1);
      int fd = (int)VAR1;
      unsigned pos = (unsigned)VAR2;

      lock_acquire(&lock_file); // ADICIONADO
      file_seek(thread_current()->DA[fd], pos);
      lock_release(&lock_file); // ADICIONADO
      break;
    }
    // ADD
    case SYS_TELL:
    {
      if (!is_valid_ptr(f->esp + 4)) exit(-1);
      int fd = (int)VAR1;
  
      if(thread_current()->DA[fd] == NULL) exit(-1);

      lock_acquire(&lock_file); // ADICIONADO
      f->eax = file_tell(thread_current()->DA[fd]);
      lock_release(&lock_file); // ADICIONADO
      break;
    }
    // ADD
    case SYS_CLOSE:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      int fd = (int)VAR1;
  
      if (fd < 3 || fd >= 128 || thread_current()->DA[fd] == NULL) 
        exit(-1);

      // 2. Protege o File System
      lock_acquire(&lock_file);
      file_close(thread_current()->DA[fd]);
      lock_release(&lock_file);

      // 3. Libera o espaço no array do processo
      thread_current()->DA[fd] = NULL;
      break;
    }

    case SYS_INUMBER:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      int fd = (int)VAR1;
      if (fd < 3 || fd >= 128 || thread_current()->DA[fd] == NULL) 
        {
          f->eax = -1;
          break;
        }

      lock_acquire(&lock_file);
      struct inode *inode = file_get_inode(thread_current()->DA[fd]);
      if (inode != NULL)
        f->eax = inode_get_inumber(inode);
      else
        f->eax = -1;
      lock_release(&lock_file);
      break;
    }

    case SYS_CHDIR:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      const char *path = (const char *)VAR1;
      check_valid_string(path);

      lock_acquire(&lock_file);
      
      /* Tenta abrir o diretório especificado pelo caminho */
      struct file *dir_file = filesys_open(path);
      struct inode *inode = dir_file ? file_get_inode(dir_file) : NULL;

      if (inode != NULL && inode_is_dir(inode)) 
        {
          /* Fecha o diretório antigo e abre o novo */
          dir_close(thread_current()->cwd);
          thread_current()->cwd = dir_open(inode_reopen(inode));
          f->eax = true;
        } 
      else 
        {
          f->eax = false;
        }

      if (dir_file)
        file_close(dir_file);

      lock_release(&lock_file);
      break;
    }
    
    case SYS_MKDIR:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      const char *path = (const char *)VAR1;
      check_valid_string(path);

      char file_name[NAME_MAX + 1];
      lock_acquire(&lock_file);
      
      struct dir *parent_dir = path_resolve(path, file_name);
      block_sector_t inode_sector = 0;
      bool success = false;

      if (parent_dir != NULL && strlen(file_name) > 0) 
        {
          /* 1. Aloca o setor no free_map e cria o diretório com a flag true (is_dir) */
          if (free_map_allocate(1, &inode_sector) && dir_create(inode_sector, 16)) 
            {
              /* 2. Adiciona o novo subdiretório no diretório pai */
              if (dir_add(parent_dir, file_name, inode_sector)) 
                {
                  /* 3. Abre o diretório recém-criado para gravar as entradas especiais . e .. */
                  struct dir *new_dir = dir_open(inode_open(inode_sector));
                  if (new_dir != NULL) 
                    {
                      /* . aponta para ele mesmo (inode_sector) */
                      /* .. aponta para o pai (obtido através do inode do pai) */
                      block_sector_t parent_sector = inode_get_inumber(dir_get_inode(parent_dir));
                      
                      dir_add(new_dir, ".", inode_sector);
                      dir_add(new_dir, "..", parent_sector);
                      dir_close(new_dir);
                      success = true;
                    }
                }
              
              /* Se algo falhou no meio do caminho, libera o setor para não haver vazamento */
              if (!success)
                free_map_release(inode_sector, 1);
            }
        }

      if (parent_dir)
        dir_close(parent_dir);

      f->eax = success;
      lock_release(&lock_file);
      break;
    }

    case SYS_ISDIR:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      int fd = (int)VAR1;
      if (fd < 3 || fd >= 128 || thread_current()->DA[fd] == NULL) 
        {
          f->eax = false;
          break;
        }

      lock_acquire(&lock_file);
      struct inode *inode = file_get_inode(thread_current()->DA[fd]);
      f->eax = (inode != NULL && inode_is_dir(inode));
      lock_release(&lock_file);
      break;
    }

    case SYS_READDIR:
    {
      if (!is_valid_ptr(f->esp + 4) || !is_valid_ptr(f->esp + 8))
        exit(-1);

      int fd = (int)VAR1;
      char *name_buffer = (char *)VAR2;

      if (fd < 3 || fd >= 128 || thread_current()->DA[fd] == NULL) 
        {
          f->eax = false;
          break;
        }

      lock_acquire(&lock_file);
      struct file *file_ptr = thread_current()->DA[fd];
      struct inode *inode = file_get_inode(file_ptr);
      
      bool success = false;
      if (inode != NULL && inode_is_dir(inode)) 
        {
          /* 1. Abre a representação de diretório */
          struct dir *dir = dir_open(inode_reopen(inode));
          if (dir != NULL) 
            {
              /* 2. Recupera o offset de leitura armazenado na struct file do processo */
              dir_seek(dir, file_tell(file_ptr));
              
              /* 3. Tenta ler o próximo registro do diretório */
              success = dir_readdir(dir, name_buffer);
              
              /* Ignora as entradas especiais "." e ".." para não listá-las ao usuário, se os testes exigirem */
              while (success && (strcmp(name_buffer, ".") == 0 || strcmp(name_buffer, "..") == 0)) {
                  success = dir_readdir(dir, name_buffer);
              }

              /* 4. Atualiza o offset de leitura de volta na struct file */
              file_seek(file_ptr, dir_tell(dir));
              
              dir_close(dir);
            }
        }

      f->eax = success;
      lock_release(&lock_file);
      break;
    }
   
    default:
      break;
  }

}