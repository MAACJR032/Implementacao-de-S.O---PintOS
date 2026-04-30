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
#include "filesys/file.h"
#include "filesys/filesys.h"

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
      file_close(cur->DA[i]);
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
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      f->eax = process_execute((const char*)VAR1);
      break;
    }

    case SYS_WRITE:
    {
      // 1. Valida se os argumentos na pilha estão em endereços válidos
      if (!is_valid_ptr((int*)f->esp + 1) || 
          !is_valid_ptr((int*)f->esp + 2) || 
          !is_valid_ptr((int*)f->esp + 3)) {
        exit(-1);
      }

      int fd = *((int*)f->esp + 1);
      void* buffer = (void*)(*((int*)f->esp + 2));
      unsigned size = *((unsigned*)f->esp + 3);

      // 2. Valida o buffer INTEIRO (Resolve o sc-boundary-3!)
      check_valid_buffer(buffer, size);

      // 3. Chama a função de escrita
      f->eax = write(fd, buffer, size);
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
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      if(!(VAR1) && !(VAR2))
        exit(-1);

      f->eax = filesys_create((const char*)VAR1,(unsigned)VAR2);
      break;
    }

    //adicionei
    case SYS_REMOVE:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      const char* file = (const char*)VAR1;
      
      // Valida se a string em si está em memória válida
      if (!is_valid_ptr(file)) {
        exit(-1);
      }
      
      // Protege o File System com o seu Lock!
      lock_acquire(&lock_file);
      f->eax = filesys_remove(file);
      lock_release(&lock_file);

      break;
    }
    
    case SYS_OPEN:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      const char *file_name = (const char*)VAR1;

      if (!is_valid_ptr(file_name))
        exit(-1);
      
      lock_acquire(&lock_file);

      struct file *file_open = filesys_open(file_name);
      int resultado = -1;

      if(file_open){
        for(int i = 3;i<128;i++){
          if(!thread_current()->DA[i]){
            if(!strcmp(thread_current()->name,file_name)){
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

    /*
    case SYS_FILESIZE:
    {

    }

    case SYS_READ:
    {

    }
    */

    case SYS_WRITE:
    {
      int fd = *((int*)f->esp + 1);
      void* buffer = (void*)(*((int*)f->esp + 2));
      if (!is_valid_ptr(buffer))
        exit(-1);
      unsigned size = *((unsigned*)f->esp + 3);
      f->eax = write(fd, buffer, size);
      break;
    }

    //adicionei
    case SYS_SEEK:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      int fd = (int)VAR1;
      unsigned pos = (unsigned)VAR2;
      if(thread_current()->DA[fd] == NULL)
        exit(-1);

      file_seek(thread_current()->DA[fd],pos);
      break;
    }


    //adicionei
    case SYS_TELL:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);
        
      int fd = (int)VAR1;
  
      if(thread_current()->DA[fd] == NULL)
        exit(-1);

      f->eax =file_tell(thread_current()->DA[fd]);
      break;
    }

    //adicionei
    case SYS_CLOSE:
    {
      if (!is_valid_ptr(f->esp + 4))
        exit(-1);

      int fd = (int)VAR1;
  
      if(thread_current()->DA[fd] == NULL)
        exit(-1);

      file_close(thread_current()->DA[fd]);
      thread_current()->DA[fd] = NULL;
      break;
    }

   
    default:
      break;
  }

}