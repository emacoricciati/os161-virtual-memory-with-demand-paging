#include "vm_tlb.h"
#include "mips/tlb.h"
#include "pt.h"
#include "spl.h"
#include "addrspace.h"
#include "opt-final.h"
#include "current.h"
#include "vm.h"
#include "vmstats.h"


#if OPT_FINAL

/*
- called when there's a TLB miss
- if we are trying to write a readonly area the process ends
- otherwise we call the IPT
*/
int vm_fault(int faulttype, vaddr_t faultaddress){

    #if OPT_DEBUG
    tlbPrint();
    #endif

    DEBUG(DB_TLB,"\nTLB fault at address: 0x%x\n", faultaddress);
    
    int spl = splhigh(); //disabling the interrupt not to block TLB update
    paddr_t paddr;
  
    faultaddress &= PAGE_FRAME; // get the address that wasn't in the TLB (removing the offset)
    incrementStatistics(FAULT);
    
    switch (faulttype)
    {
    case VM_FAULT_READ: //reading from address that is not in TLB
        break;
    case VM_FAULT_WRITE: //write to an address not in TLB
        break;
    
    //The text segment cannot be written by the process -> the process has to be ended by means of a syscall
    //(no need to panic, kernel should not crash) 
    case VM_FAULT_READONLY:
        kprintf("Attempted to write to a read-only segment. Terminating process...");
        sys__exit(0);
        break;
    default:
        break;
    }
    //Check if the address space is setted up correctly
    KASSERT(as_is_correct() == 1);
    //Get physical address that it's not present in the TLB from the Page Table
    paddr = getFramePT(faultaddress);
    //Insert address into the TLB
    tlbInsert(faultaddress, paddr);
    splx(spl); //restoring the interrupts
    return 0;
}
#endif 

/*
Write a new entry into the TLB.
- input parameters: the fault address (virtual) and physical address (given by the page table)
 */
int tlbInsert(vaddr_t faultvaddr, paddr_t faultpaddr){
    //faultpaddr is the address of the beginning of the physical frame, so I have to remember that I do not have to 
    //pass the whole address but I have to mask the least significant 12 bits
    int entry, valid, isRO; 
    uint32_t hi, lo, prevHi, prevLo;
    isRO = segmentIsReadOnly(faultvaddr);

    //Search for an available entry
    for(entry = 0; entry <NUM_TLB; entry++){
        valid = tlbEntryIsValid(entry);
        if(!valid){
            //Write the entry in the TLB
                hi = faultvaddr;
                lo = faultpaddr | TLBLO_VALID; //the entry has to be set as valid
               if(!isRO){
                    lo = lo | TLBLO_DIRTY; //Set a dirty bit (write privilege)
                }
            tlb_write(hi, lo, entry);
            incrementStatistics(FAULT_WITH_FREE);
            return 0;
        }

    }

    //Look for a victim
    entry = tlbVictim();
    hi = faultvaddr;
    lo = faultpaddr | TLBLO_VALID; //the entry has to be set as valid
    if(!isRO){
        lo = lo | TLBLO_DIRTY;  //Set a dirty bit (write privilege)
    }
    //notify the PT that the entry with that virtual address is not in TLB anymore
    tlb_read(&prevHi, &prevLo, entry);
    tlbUpdateBit(prevHi, curproc->p_pid); 

    //Overwrite the content
    tlb_write(hi, lo, entry);
    incrementStatistics(FAULT_WITH_REPLACE);
    return 0;

}

/*
Print the content of the TLB.
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
Returns the index of the victim selected (Round Robin) in the TLB.
*/ 
int tlbVictim(void){                               
    int vict;       

    static unsigned int next_vict = 0;

    vict = next_vict;
    next_vict = (next_vict + 1) % NUM_TLB;//the constant is the number of TLB entries in the processor

    return vict;
}

/*
Defines if, given the virtual address of a frame, it is read only or not.
*/
int segmentIsReadOnly(vaddr_t virtualAddr){
    struct addrspace *as;

    as = proc_getas();//Get the address space of the process

    int isReadOnly = 0;

    uint32_t firstTextVirtAddr = as->as_vbase1; //starting address of the text segment
    int sizeFrame = as->as_npages1;

    uint32_t lastTextVirtAddr = (sizeFrame * PAGE_SIZE) +  firstTextVirtAddr; //ending address of the text segment

    //Check if the virtual address is in the range of the text segment
    if((virtualAddr >= firstTextVirtAddr) && (virtualAddr <= lastTextVirtAddr)){
        isReadOnly = 1;        
    }

    return isReadOnly;
}

/*
Check if the entry in the TLB at index i is valid or not.
*/
int tlbEntryIsValid(int i){
    uint32_t hi, lo;
    tlb_read(&hi, &lo, i);
    return (lo & TLBLO_VALID); //result == 0 --> entry invalid
}

/*
Invalidate the TLB when there is a switch from a process to another (there is no PID field).
*/
void tlbInvalidate(void){
    uint32_t hi, lo;
    pid_t pid = curproc->p_pid;

    // The process changed, not just the thread. This matters because as_activate is also triggered by thread changes.
    if(previous_pid != pid)
    {
    DEBUG(DB_TLB,"New process executing: %d replacing %d. Invalidating TLB entries\n", pid, previous_pid);
    incrementStatistics(INVALIDATION);
    
    // Invalidate entries
    for(int i = 0; i<NUM_TLB; i++){
        if(tlbEntryIsValid(i)){
            tlb_read(&hi,&lo,i);
            tlbUpdateBit(hi,previous_pid); //tell the PT that the entry is not in the TLB anymore
        }
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i); // Invalidate the entry
    }
    previous_pid = pid; // Update the pid
    }
}