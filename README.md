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
```c
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

```c
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

```c
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

# ADDRSPACE

The code related to this section can be found in the following files:

```bash
kern/include/addrspace.h
kern/vm/addrspace.c
kern/syscall/loadelf.c
```

In the base version of OS/161, when the OS loads a new program into an address space using `runprogram`, it pre-allocates physical frames for all of the program’s virtual pages, and it pre-loads all of the pages into physical memory. 

With "On demand" paging, a page should be loaded (and physical space should be allocated for it) the first time the application tries to use (read or write) that page. Pages that are never used by an application should never be loaded into memory and should not consume a physical frame.

To implement this, we made some modifications to the address space handling:

```c
struct addrspace {
    //text
    vaddr_t as_vbase1;      
    size_t as_npages1;
    size_t initial_offset_text;                                                     
    Elf_Phdr prog_head_text;  // Program header of the text section                    
    //data
    vaddr_t as_vbase2;     
    size_t as_npages2;
    size_t initial_offset_data;                                                     
    Elf_Phdr prog_head_data;  // Program header of the data section               
    struct vnode *v;           // vnode of the ELF file                                 
    int valid;
};
```

In more detail, we store in the address space the program headers related to the text and data segments, and also the vnode of the ELF file. These fields allow us to perform on-demand paging, reading specific parts of the ELF file only when needed and loading pages into physical memory.

Additionally, there are other fields: `initial_offset_text` and `initial_offset_data`, which are important for identifying the correct first address of a program that may not be aligned with the page boundary. The full explanation of how to handle these offsets is provided in the [segment section](#segments).

As for the stack, the field `stackpbase` is no longer included in the address space, as it's now completely handled at runtime.

In order to support "On demand" paging, some functions were modified. Here is a list of the main modified functions with their differences compared to the base implementation:

- **as_copy**: It's called during a fork operation. It increases the reference count on the vnode of the ELF file and calls functions to duplicate entries in the page table and swap file for the new related pid.
- **as_destroy**: It closes the ELF file only when the reference count is `0`, otherwise it decreases the counter.
- **as_activate**: It calls `tlbInvalidate` to invalidate all entries in the TLB when a new process is running.
- **as_define_region**: It computes the initial offset using the formula:
  
  ```c
  initial_offset = vaddr % PAGE_SIZE;
  ```

  Then, it stores the value in the address space and uses it to compute the correct `memsize`.

- **alloc_kpages**: It performs the allocation of contiguous kernel pages depending on the `pt_active` value:
  - If `pt_active = 1`, it uses the Page Table with the function `getContiguousPages`.
  - Otherwise, it "steals" pages from physical memory using `getppages`.

- **free_kpages**: It frees kernel pages only when `pt_active = 1`. In other words, we accept a small memory leak for what is allocated before the initialization of the Page Table. Adding a new data structure to address this issue would have been too costly.

- **vm_shutdown**: It prints the statistics defined in the [statistics section](#statistics).

# SEGMENTS

The code related to this section can be found in the following files:

```bash
kern/include/segments.h
kern/vm/segments.c
```

In this section, we examine the proper handling of page faults. When a page fault occurs, our task is to load a page into a specified physical address. This loading can originate either from the ELF file or the swap file.

Loading from the ELF file happens when we access a particular page for the first time. In contrast, loading from the swap file occurs when the page was previously loaded from the ELF file but has since been swapped out.

To differentiate between these two scenarios, the function `loadPage` checks whether the page has already been loaded from the ELF file. To facilitate this check, we use a flag variable named `found`. If `found` is set to `0`, it indicates that the page is not present in the swap file, necessitating a load from the ELF file.


```c
int loadPage(vaddr_t vaddr, pid_t pid, paddr_t paddr){
    int found, result;
    struct addrspace *as;
    int sz=PAGE_SIZE, memsz=PAGE_SIZE;
	size_t additional_offset=0;

    found = loadSwapFrame(vaddr, pid, paddr); 

    if(found){
        return 0;
    }
    ...
}
```
If the page is not present in the Page Table, we need to load it from the ELF file. First, we must determine which segment the page belongs to:
- If it is a text or data page, we load the page using the `loadELFPage` function.
- If it is a stack page, we simply zero-fill the page.

In order to load pages from the ELF file, we store the program headers for the text and data segments in the address space data structure. This allows us to compute the correct offset within the file based on the virtual address being accessed. The formula used for this is:

```c
offset = as->ph.offset + (vaddr - as->as_vbase);
```

`vaddr` is the virtual address being accessed, while `as->as_vbase` is the starting address of the segment. This way, `(vaddr - as->as_vbase)` gives the correct offset within the file relative to the appropriate program header.

It's important to remember that the process won't necessarily access the pages of the ELF file in a sequential order. There may be jumps in the text segment, or variables could be accessed in a different order than they were declared, meaning we can't rely on sequential access.

However, even though the ELF file may not be read sequentially, we can be certain that each page will only be loaded once. If a page has already been read, it will be available either in the page table or the swap file, which are checked before accessing the ELF file again as described before.

It's also important to understand the difference between `memsz` and `filesz`:

- `memsz`: size of the segment in memory.
- `filesz`: size of the segment in the ELF file.

There are three possible cases to consider:

- **`filesz` < `memsz`**: This occurs when a program includes only the declaration of a global variable without initialization (e.g., in the `testbin/zero` test).
For example:
```c
  #include "stdio.h"
  int big_array[1000];
  ...
```
In this case, `big_array` is not present in the ELF file but it will be defined in memory
- **`filesz` = `memsz`**: This happens when a global variable is declared and initialized in the program.
```c
#include "stdio.h"
int array[5] = {1,2,3,4,5}
...
```
Here, `array` is present both in the ELF file and will be loaded into memory.
- **`filesz` > `memsz`**:This scenario is not possible, as the file size cannot exceed the memory size defined for a segment.

To deal with this problem, it's important to compute the size as:
```c
sz = as->ph.filesz - (vaddr - as->as_vbase);
```
This calculation of `sz` gives us the exact number of bytes that need to be loaded from the file:
- If **`sz` > `PAGE_SIZE`**, we set `sz=PAGE_SIZE` as we cannot read more than a page.
- If **`sz` < `0`**, we simply fill the page with zeros and return, which typically occurs when `memsz` > `filesz`
- If **`sz` > `0` && `sz` < `PAGE_SIZE`**, we first zero-fill the page and then load the `sz` bytes, which can happen when the last page contains unused space due to internal fragmentation.

Finally, as anticipated in the [address space section](#ADDRSPACE), the starting virtual address may not align with a page boundary. In such cases, it is essential to take the `initial_offset` into account when accessing the first page. If the `initial_offset` is not zero, we need to zero-fill the page first (to ensure the initial offset bits are set to 0) and then begin loading data from the specified `initial_offset` within the block, rather than starting from the usual position of 0.














