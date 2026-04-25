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

static bool
is_valid_ptr(const void *ptr)
{
    return ptr != NULL
        && is_user_vaddr(ptr)
        && pagedir_get_page(thread_current()->pagedir, ptr) != NULL;
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
  if(!is_valid_ptr(f->esp)) {
    exit(-1);
  }
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
      if (!is_valid_ptr((void*)f->esp + 4))
        exit(-1);

      exit((int)VAR1);
      break;
    }
    //adicionei
    case SYS_EXEC:
    {
      if (!is_valid_ptr((int*)f->esp + 4))
        exit(-1);

      f->eax = process_execute((const char*)VAR1);
      break;
    }

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
    
    //ajustei
    case SYS_WAIT:
    {
      if (!is_valid_ptr((int*)f->esp + 4))
        exit(-1);

      f->eax = process_wait((tid_t)VAR1);
      break;
    }

    //adicionei
    case SYS_REMOVE:
    {
      if (!is_valid_ptr((int*)f->esp + 4))
        exit(-1);

      const char* file = (const char*)VAR1;
      if(!file){
        exit(-1);
      }
      
      f->eax = filesys_remove(file);

      break;
    }

    default:
      break;
  }

}