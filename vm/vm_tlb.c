#include "vm_tlb.h"
#include "mips/tlb.h"
#include "pt.h"
#include "spl.h"
#include "addrspace.h"
#include "opt-final.h"
#include "current.h"
#include "vm.h" // includes the definition of vm_fault
#include "stats.h"


#if OPT_FINAL

int vm_fault(int faulttype, vaddr_t faultaddress){

    #if OPT_DEBUG
    tlbPrint();
    #endif

    DEBUG(DB_IPT,"\nFault address: 0x%x\n",faultaddress);
    int spl = splhigh();
    paddr_t paddr;
  
    faultaddress &= PAGE_FRAME; // get the address that wasn't in the TLB (removing the offset)

    incrementStatistics(FAULT); // update the statistics
    
    switch (faulttype)
    {
    case VM_FAULT_READ:
        break;
    case VM_FAULT_WRITE:
        break;
    /*The text segment cannot be written by the process. 
    If the process tries to modify a RO segment, the process has to be ended by means of an 
    appropriate exception (no need to panic, kernel should not crash)*/
    case VM_FAULT_READONLY:
        kprintf("Attempted to write to a read-only segment. Terminating process...");
        sys__exit(0); // TODO: try to exit using VM_FAULT_READONLY
        break;
    default:
        break;
    }
    /*Check if the address space is setted up correctly*/
    KASSERT(as_is_correct() == 1);
   /*Get physical address that it's not present in the TLB from the Page Table*/
    paddr = getFramePT(faultaddress);
    /*Insert address into the TLB */
    tlbInsert(faultaddress, paddr);
    splx(spl);
    return 0;
}
#endif 

/**
 * Write a new entry into the TLB. This function takes the fault address (virtual)
 * and the corresponding physical address provided by the page table.
 * It locates an available slot for the entry (vaddr, paddr) using the tlbVictim function.
 */
int tlbInsert(vaddr_t faultvaddr, paddr_t faultpaddr){
    /*faultpaddr is the address of the beginning of the physical frame, so I have to remember that I do not have to 
    pass the whole address but I have to mask the least significant 12 bits*/
    int entry, valid, isRO; 
    uint32_t hi, lo, prevHi, prevLo;
    isRO = segmentIsReadOnly(faultvaddr);

    /* Step 1: Search for an available entry, update the statistic (free)*/
    for(entry = 0; entry <NUM_TLB; entry++){
        valid = tlbEntryIsValid(entry);
        if(!valid){
            /*Write the entry in the TLB*/
                hi = faultvaddr;
                lo = faultpaddr | TLBLO_VALID; //the entry has to be set as valid
               if(!isRO){
                    /*Set a dirty bit (write privilege)*/
                    lo = lo | TLBLO_DIRTY; 
                }
            //lo = lo | TLBLO_DIRTY;  // TODO: try later (on demand paging)
            tlb_write(hi, lo, entry);
            // update the statistic
            incrementStatistics(FAULT_WITH_FREE);
            return 0;
        }

    }
    /*Step 2: Invalid entry not found. Look for a victim, overwrite and update the statistic (replace)*/
    entry = tlbVictim();
    hi = faultvaddr;
    lo = faultpaddr | TLBLO_VALID; //the entry has to be set as valid
    if(!isRO){
        /*Set a dirty bit (write privilege)*/
        lo = lo | TLBLO_DIRTY; 
    }
    /*notify the PT that the entry with that virtual address is not in TLB anymore*/
    tlb_read(&prevHi, &prevLo, entry); // read the content of the entry
    tlbUpdateBit(prevHi, curproc->p_pid); // update the PT
    /*Overwrite the content*/
    tlb_write(hi, lo, entry);
    // Update statistic
    incrementStatistics(FAULT_WITH_REPLACE);
    return 0;

}
/**
 * Used for debugging. Print the content of the TLB.
*/
void tlbPrint(void){
    uint32_t hi, lo;

    kprintf("\n\n\tTLB\n\n");

    for(int i = 0; i<NUM_TLB; i++){
        tlb_read(&hi, &lo, i);
        kprintf("%d virtual: 0x%x, physical: 0x%x\n", i, hi, lo);
    }

}

/*
Chooses and returns the index of the entry to sacrifice in the TLB.
*/ 
int tlbVictim(void){                               //this soultion uses the entry to invalidate exploiting the Round Robin policy
    int vict;       

    static unsigned int next_vict = 0;

    vict = next_vict;
    next_vict = (next_vict + 1) % NUM_TLB;          //the constant is the number of TLB entries in the processor

    return vict;
}

/*
Defines if, given the virtual address of a frame, it is read only or not.
*/
int segmentIsReadOnly(vaddr_t virtualAddr){
    struct addrspace *as;

    as = proc_getas();              //Get the address space of the process (substitute for the moment that function if it isn't developed yet)

    int isReadOnly = 0;

    uint32_t firstTextVirtAddr = as->as_vbase1;
    int sizeFrame = as->as_npages1;

    uint32_t lastTextVirtAddr = (sizeFrame * PAGE_SIZE) +  firstTextVirtAddr;

    //Check if the virtual address is in the range of the virtual address that has been assigned to the text segment
    if((virtualAddr >= firstTextVirtAddr) && (virtualAddr <= lastTextVirtAddr)){
        isReadOnly = 1;         //If it is in the range we set to 1
    }

    return isReadOnly;
}

/**
 * Checkif the entry in the TLB at index i is valid or not.
*/
int tlbEntryIsValid(int i){
    uint32_t hi, lo;
    tlb_read(&hi, &lo, i);
    return (lo & TLBLO_VALID);
}

/**
 * Invalidate the TLB when there is a switch from a process to another. Indeed, 
 * the TLB is common for all processes and does not have a "pid" field.
*/
void tlbInvalidate(void){
    uint32_t hi, lo;
    pid_t pid = curproc->p_pid;

    // The process changed, not just the thread. This matters because as_activate is also triggered by thread changes.
    if(previous_pid != pid) 
    {
    DEBUG(DB_VM,"NEW PROCESS RUNNING: %d INSTEAD OF %d\n",pid,previous_pid);

    // Update statistic
    incrementStatistics(INVALIDATION);

    // Invalidate entries
    for(int i = 0; i<NUM_TLB; i++){
        if(tlbEntryIsValid(i)){
            tlb_read(&hi,&lo,i);
            tlbUpdateBit(hi,previous_pid); // Entry of the PT is not cached anymore
        }
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i); // Invalidate the entry
    }
    previous_pid = pid; // Update the pid
    }
}