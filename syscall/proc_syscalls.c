/*
 * AUthor: G.Cabodi
 * Very simple implementation of sys__exit.
 * It just avoids crash/panic. Full process exit still TODO
 * Address space is released
 */

#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <current.h>
#include <synch.h>
#include "swapfile.h"
#include "opt-final.h"

#if OPT_FINAL
#include "pt.h"
#endif
/*
 * system calls for process management
 */
void
sys__exit(int status)
{
  struct proc *p = curproc;
  #if OPT_FINAL
  freePages(p->p_pid);
  freeProcessPagesInSwap(p->p_pid);                       //Added
  #endif

  DEBUG(DB_VM,"Process %d ending\n",curproc->p_pid);      //Added

  p->p_status = status & 0xff; /* just lower 8 bits returned */
  proc_remthread(curthread);
  lock_acquire(p->lock);
  cv_signal(p->p_cv, p->lock);
  lock_release(p->lock);
  thread_exit();

  panic("thread_exit returned (should not happen)\n");
}

int
sys_waitpid(pid_t pid, userptr_t statusp, int options)
{
  struct proc *p = proc_search_pid(pid);
  int s;
  (void)options; /* not handled */
  if (p==NULL) return -1;
  s = proc_wait(p);
  if (statusp!=NULL) 
    *(int*)statusp = s;
  return pid;
}

pid_t
sys_getpid(void)
{
  KASSERT(curproc != NULL);
  return curproc->p_pid;
}

#if OPT_FORK
static void
call_enter_forked_process(void *tfv, unsigned long dummy) {
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf); 
 
  panic("enter_forked_process returned (should not happen)\n");
}

int sys_fork(struct trapframe *ctf, pid_t *retval) {
  struct trapframe *tf_child;
  struct proc *newp;
  #if OPT_FINAL
  pid_t old, new;
  #endif
  int result;

  KASSERT(curproc != NULL);

  newp = proc_create_runprogram(curproc->p_name);
  if (newp == NULL) {
    return ENOMEM;
  }

  #if OPT_FINAL
  old=curproc->p_pid;
  new=newp->p_pid;
  #endif

  /* done here as we need to duplicate the address space 
     of thbe current process */
  #if OPT_FINAL
  newp->ended=0;
  as_copy(curproc->p_addrspace, &(newp->p_addrspace), old, new); //Copy the address space
  if(newp->p_addrspace == NULL){
    proc_destroy(newp); 
    return ENOMEM; 
  }
  #else
  as_copy(curproc->p_addrspace, &(newp->p_addrspace));
  if(newp->p_addrspace == NULL){
    proc_destroy(newp); 
    return ENOMEM; 
  }
  #endif

  /* we need a copy of the parent's trapframe */
  tf_child = kmalloc(sizeof(struct trapframe));
  if(tf_child == NULL){
    proc_destroy(newp);
    return ENOMEM; 
  }
  memcpy(tf_child, ctf, sizeof(struct trapframe));

  /* TO BE DONE: linking parent/child, so that child terminated 
     on parent exit */

  result = thread_fork(
		 curthread->t_name, newp,
		 call_enter_forked_process, 
		 (void *)tf_child, (unsigned long)0/*unused*/);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
    return ENOMEM;
  }

  *retval = newp->p_pid;

  return 0;
}
#endif
