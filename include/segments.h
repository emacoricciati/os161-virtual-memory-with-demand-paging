#ifndef _SEGMENTS_H_
#define _SEGMENTS_H_

#include "swapfile.h"
#include "syscall.h"
#include "vm.h"
#include "proc.h"
#include "types.h"
#include "addrspace.h"
#include "elf.h"
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <elf.h>
#include "vmstats.h"
#include "opt-final.h"
#include "opt-debug.h"

/**
 * Given a virtual address (vaddr), this function locates the corresponding page and loads it into the provided paddr.
 * 
 * @param vaddr: the virtual address that triggered the page fault
 * @param pid: the process ID of the process that caused the page fault
 * @param paddr: the physical address where the page will be loaded
 * 
 * @return 0 if the operation succeeds, otherwise returns the error code from VOP_READ
 */
int loadPage(vaddr_t vaddr, pid_t pid, paddr_t paddr);

#endif