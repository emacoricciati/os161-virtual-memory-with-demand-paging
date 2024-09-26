/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include "pt.h"
#include "current.h"
#include "spl.h"
#include "mips/tlb.h"
#include <cpu.h>
#include "opt-final.h"
#include "vm_tlb.h"
#include "vmstats.h"
#include "vfs.h"
#include "vnode.h"
#include "swapfile.h"

#if OPT_FINAL
#include "pt.h"
#endif

#define DUMBVM_STACKPAGES    18
/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

#if OPT_DUMBVM
static
void
dumbvm_can_sleep(void)
{
	if (CURCPU_EXISTS()) {
		/* must not hold spinlocks */
		KASSERT(curcpu->c_spinlocks == 0);

		/* must not be in an interrupt handler */
		KASSERT(curthread->t_in_interrupt == 0);
	}
}
#endif

static
paddr_t
getppages(unsigned long npages)//allocates pages from RAM
{
	paddr_t addr;

	addr = ram_stealmem(npages);

	return addr;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_npages2 = 0;

	return as;
}

int
as_copy(struct addrspace *src, struct addrspace **ret, pid_t oldPid, pid_t newPid){
	
	struct addrspace *newAddrSpace;

	newAddrSpace = as_create();

	if(newAddrSpace == NULL){
		return ENOMEM;
	}

	//Assigning old address space to the new address space
	newAddrSpace->as_vbase1 = src->as_vbase1;
	newAddrSpace->as_npages1 = src->as_npages1;
	newAddrSpace->as_vbase2 = src->as_vbase2;
	newAddrSpace->as_npages2 = src->as_npages2;

	//Copying the program headers
	newAddrSpace->prog_head_text = src->prog_head_text;
	newAddrSpace->prog_head_data = src->prog_head_data;

	//Copying the vnode related to the ELF file
	newAddrSpace->v = src->v;

	//As the file is owned by an additional process we have to increase the reference counter in the vnode
	src->v->vn_refcount++;	//in order to ensure a safe close of the ELF file

	newAddrSpace->initial_offset_text = src->initial_offset_text;
	newAddrSpace->initial_offset_data = src->initial_offset_data;

	prepareCopyPT(oldPid);			
	duplicateSwapPages(newPid, oldPid);		//Copying the swap pages
	copyPTEntries(oldPid, newPid);		//Copying the entries of the Page Table
	endCopyPT(oldPid);				

	*ret = newAddrSpace;
	return 0;
}

//dispose of an address space. You may need to change the way this works if implementing user-level threads.
void as_destroy(struct addrspace *as){
	if(as->v->vn_refcount==1){	//if there is only one process related to the elf file
		vfs_close(as->v); 		//closing the ELF file by sys_exit on the last process owning it
	}
	else{
		as->v->vn_refcount--; 	//decreasing the number of processes related to the ELF file
	}

	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;
	int spl;
	spl = splhigh();
	DEBUG(DB_EXEC,"Process %d running\n",curproc->p_pid);
	as = proc_getas();
	if (as == NULL) {
		return;
	}

	// invalidate entries in the TLB only when a new process is activated
	tlbInvalidate();
	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

//set up a region of memory within the address space.
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize, int readable, int writeable, int executable){
	size_t npages, initial_offset;

	//align the base of the region
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;	//calculates the offset of vaddr within its current page. This is added to memsize to account for the extra space needed to align vaddr to the page boundary.
	initial_offset=vaddr % PAGE_SIZE; 			//since vaddr may not be aligned to a page, we save the initial offset (that otherwise would be lost after the next instruction)
	vaddr &= PAGE_FRAME;						//aligns the vaddr to the start of the page by clearing the offset bits

	//align the length of the region
	memsize = (memsize + initial_offset + PAGE_SIZE - 1) & PAGE_FRAME;	//Adds initial_offset to account for the part of the page that was "lost" due to alignment.
																		//Adds PAGE_SIZE - 1 to ensure any partial page is fully covered.
																		//Aligns the result to the nearest page boundary using & PAGE_FRAME
	npages = memsize / PAGE_SIZE;

	//We don't use these - exceptions about writing a readonly page will be raised by checking the virtual address
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {								//region not yet defined
		DEBUG(DB_VM,"\nText starts at: 0x%x\n",vaddr);
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		as->initial_offset_text=initial_offset;
		return 0;
	}

	if (as->as_vbase2 == 0) {								//region not yet defined
		DEBUG(DB_VM,"Data starts at: 0x%x\n",vaddr);
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		as->initial_offset_data=initial_offset;
		return 0;
	}

	kprintf("Too many regions at once\n");	//only one region at a time is possible

	return ENOSYS;
}

// Not used with on demand paging
// static
// void
// as_zero_region(paddr_t paddr, unsigned npages)	
// {
// 	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
// }

// This function is not needed with on demand paging (pages are loaded only when needed)
int
as_prepare_load(struct addrspace *as)
{

	(void)as;
	return 0;
}

int as_complete_load(struct addrspace *as){         //it's not needed on demand paging as we will not load anything without a fault
    (void)as;
    return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr){
	(void)as;								//common way to suppress compiler warnings about unused parameters
	*stackptr = USERSTACK;					
	return 0;
}


void vm_bootstrap(void){
	initSwapfile();
	initPT();
	initializeStatistics();
}

void addrspace_init(void){
	spinlock_init(&stealmem_lock);	//used to protect critical sections that deal with memory, in situations where memory is being allocated directly 
									//(before the virtual memory system is fully operational)
	pt_active=0;
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{

	int spl = splhigh(); // avoid context switch in this phase

	paddr_t pa;

	spinlock_acquire(&stealmem_lock);

	if(!pt_active){
		pa = getppages(npages);
	}
	else {
		spinlock_release(&stealmem_lock);
		pa = getContiguousPages(npages);
		spinlock_acquire(&stealmem_lock);
	}
	if (pa==0) {
		return 0;
	}
	spinlock_release(&stealmem_lock);
	splx(spl);
	return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr){
    int spl = splhigh();

    spinlock_acquire(&stealmem_lock);

    if(!pt_active || addr < PADDR_TO_KVADDR(pt_info.firstfreepaddr)){
        //Accepting a memory leak because having an additional data structure would be more expensive
        //than a potential memory leak and also can never be freed.
    }
    else{
        spinlock_release(&stealmem_lock);
        freeContiguousPages(addr);
        spinlock_acquire(&stealmem_lock);
    }

    spinlock_release(&stealmem_lock);

    splx(spl);
}

int as_is_correct(void){
    struct addrspace *as = proc_getas();
    if(as == NULL)
        return 0;
    if(as->as_vbase1 == 0)
        return 0;
    if(as->as_vbase2 == 0)
        return 0;
    if(as->as_npages1 == 0)
        return 0;
    if(as->as_npages2 == 0)
        return 0;
    return 1;
}

void vm_shutdown(void){
	
	// print statistics
	printStatistics();
}

void createSemFork(void){
	sem_fork = sem_create("sem_fork",1);
}