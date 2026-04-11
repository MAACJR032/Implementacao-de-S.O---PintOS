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

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Implementação mínima de exit para o kernel */
void exit(int status)
{
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

  int syscall_num = *(int *) f->esp;
  switch (syscall_num)
  {
    case SYS_WRITE:
    {
      int fd = *((int*)f->esp + 1);
      void* buffer = (void*)(*((int*)f->esp + 2));
      unsigned size = *((unsigned*)f->esp + 3);
      f->eax = write(fd, buffer, size);
      break;
    }
    
    case SYS_EXIT:
    {
      int status = *((int*)f->esp + 1);
      exit(status);
      // Não retorna
      break;
    }
    
    case SYS_WAIT:
    {
      int child_tid = *((int*)f->esp + 1);
      f->eax = process_wait(child_tid);
      break;
    }

    default:
      break;
  }
}
