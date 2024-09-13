# OS161-VIRTUAL MEMORY WITH DEMANDING PAGING

This project, realized by [Emanuele Coricciati](https://github.com/emacoricciati), [Erika Astegiano](https://github.com/astegiano-erika) and [Giacomo Belluardo](https://github.com/giacomobelluardo), will take care of implementing virtual memory with on demand paging on OS161 and error handling for readonly segments and segmentation fault.

# TLB Management and vm_faults
The code related to this section can be found at:

```bash
kern/include/vm.h

kern/include/vm_tlb.h

kern/vm/vm_tlb.c
```
### vm_fault
The function *****vm_fault***** is defined  in *****vm_tlb.c*****. It's called by a trap code every time there is a TLB miss.

Inside the function, the code checks if the fault was caused by an attempt of writing on a read only area. If it wasn't writing on the read only area, the page table will provide the physical address related to the virtual address that have caused the fault, calling `getFramePT` function.
Otherwise, the process is ended.

Finally, the physical address will be inserted in the TLB using `tlbInsert` function.
```jsx
int vm_fault(int faulttype, vaddr_t faultaddress){

    #if OPT_DEBUG
    tlbPrint();
    #endif

    DEBUG(DB_VM,"\nTLB fault at address: 0x%x\n", faultaddress);
    
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
```

### tlbInsert
The function will insert a new entry in the TLB. It takes two parameters: 
1. faultvaddr, which represents the address of the fault
2. faultpaddr, which represents the address of the beginning of the physical frame (with the least significant 12 bits marked)

At first, the function will search for an available entry (invalid entry), and if it's found it will write it in the TLB.

If there's no invalid entry, the function will look for a victim (by means of Round Robin algorithm). Then the function will return the index and the entry will be overwritten with the new value.

```js
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
```

### tlbInvalidate
This function invalidats the TLB when there is a switch from a process to another. 

More in details, it first checks if the current process has changed its pid. If so, it will write the invalidating values in all of the TLB entries. 
However before writing the values the TLB will inform the Page Table that each entry won't be cached anymore.

```js
void tlbInvalidate(void){
    uint32_t hi, lo;
    pid_t pid = curproc->p_pid;

    // The process changed, not just the thread. This matters because as_activate is also triggered by thread changes.
    if(previous_pid != pid) 
    {
    DEBUG(DB_VM,"New process executing: %d replacing %d. Invalidating TLB entries\n", pid, previous_pid);

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
```