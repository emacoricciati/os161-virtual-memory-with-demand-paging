# OS161-VIRTUAL MEMORY WITH DEMANDING PAGING

This project, realized by [Emanuele Coricciati](https://github.com/emacoricciati), [Erika Astegiano](https://github.com/astegiano-erika) and [Giacomo Belluardo](https://github.com/giacomobelluardo), will take care of implementing virtual memory with on demand paging on OS161 and error handling for readonly segments and segmentation fault.

To use our version of OS161, it is recommended to increase `ramsize` to `2M` in the `sys161.conf` file to reduce the number of swap operations performed on the swap file. The swap file was implemented using the raw partition `LHD0.img`, resizing it to `9M`. You can change the size of the partition with the following command in the root folder:

```bash
disk161 resize LHD0.img 9M
```
This implementation was tested using the following tests from the `testbin` folder:
- palin
- matmult
- huge
- sort
- forktest
- parallelvm
- bigfork

Additionally, the `testbin/zero` test was run for specific reasons explained later. However, it doesn't fully work since the `sbrk` syscall is not implemented in this version of OS161.

### DEBUGGING THE SYSTEM

To track the flow of operations in the OS, debug prints were introduced in the code. The debugging process was carried out using:
- **Debug option**: It prints the TLB state each time `vm_fault` is called, and prints the page list every time functions like `copyPTEntries`, `loadPage`, and `loadSwapFrame` are invoked. It can be enabled in the `conf/FINAL` file by simply uncommenting the respective line
```bash
#options debug
```
- **Debug flow prints**: Special debug flags, namely `DB_VM`,`DB_IPT`, `DB_TLB`, and `DB_SWAP`, were used to log significant events for each of these features. These flags can be configured in the `kprintf.c` file by modifying the `dbflags` variable, which has a default value of 0. You can enable multiple flags at once by using the logical OR operator (`|`) between different flags.

# TLB Management
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

    DEBUG(DB_TLB,"\nTLB fault at address: 0x%x\n", faultaddress);
    
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
        sys__exit(0);
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
    DEBUG(DB_TLB,"New process executing: %d replacing %d. Invalidating TLB entries\n", pid, previous_pid);

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

# INVERTED PAGE TABLE

The code related to this section can be found in the following files:
```bash
kern/vm/pt.h  
kern/vm/pt.c  
```

The project uses an inverted page table (IPT), where there is one entry for each physical page in memory. Each entry holds the virtual address of the page stored at that physical memory location, along with information about the process that owns the page. This reduces the memory required to store the page table compared to traditional approaches.
The downside is that searching for a page requires a linear scan of the entire page table, which can slow down lookups.
One possible solution to speed up the search is to use a hash map, which is not implemented in this project.

The victim selection policy is the FIFO replacement algorithm with a second chance. In this algorithm, only removable pages are considered; pages allocated with `kmalloc`, involved in I/O or fork operations, or cached in the TLB are ignored. 

The page table structure is as follows:

```c
// Page table entry
struct pt_entry_s {             
    pid_t pid;      // Process ID   
    vaddr_t vPage;  // Virtual page
    uint8_t ctl;    // Control bits: Validity bit, Reference bit, Kalloc bit
} pt_entry;

// Page table
struct pt_info_s {
    struct pt_entry_s *pt;  // IPT
    int ptSize;             // IPT size: number of page entries
    paddr_t firstfreepaddr; // IPT starting physical address
    struct lock *pt_lock;   // IPT lock
    struct cv *pt_cv;       // IPT condition variable
    int *allocSize;         // Number of allocated pages to free
} pt_info;

int pt_active;
```
Using this structure, the physical address is computed with the following formula:
```c
p_addr = pt_info.firstfreepaddr + index * PAGE_SIZE
```
The control bits for each entry include:
- **Validity bit**: Determines if a page table entry is valid.
- **Reference bit**: Used for the second chance page replacement algorithm.
- **Kmalloc bit**: Identifies pages allocated with `kmalloc`, which cannot be swapped out.
- **TLB bit**: Indicates if a page is cached in the TLB.
- **IO bit**: Shows if a page is involved in I/O operations with the disk.
- **Swap bit**: Shows if a page is involved in fork operations.

The `allocSize` array helps track the number of pages allocated starting at the i-th page, which is important for freeing contiguous pages in memory.
The `pt_active` variable is used to check if the page table is currently active. This is necessary because some operations might be executed before the page table is initialized.

### Main functions

- **initPT**: This function is called by `vm_bootstrap` during the initialization of the system. It allocates all the data structures needed to manage the Page Table. The allocation is done using `kmalloc`, so the data structures allocated at this point are not managed by the Page Table. It computes the number of frames available in RAM to determine the size of the PT. The `firstfreepaddr` is computed using `ram_stealmem(0)`, which retrieves 0 bytes from physical memory. The final step is to set `pt_active` to 1.
- **getFramePT**: It's a wrapper for `getPAddressPT` that receives the `vaddr` of a page and returns the corresponding physical address in the Page Table.  
  It searches for the virtual address in the Page Table:
  - If found, it returns the physical address.
  - Otherwise, it searches for a free entry in the Page Table; if none are present, it selects a victim using `findVictim`.  
In both cases, it loads a page from either the ELF or the swap file using `loadPage`, and it sets `VALBIT=1` and `IOBIT=1`.
- **getFramePT**: It's called by `getFramePT`, receiving the `pid` of the process and the `vaddr`. It returns:
  - `-1` if not found
  - `paddr` if found  
It also sets `TLBBIT = 1` since the entry will be cached in the TLB.
- **findFreeEntryPT**: It's called by `getFramePT` and returns the first free entry available in the Page Table. An entry is considered free if `VALBIT=KBIT=IOBIT=SWAPBIT=0`.
- **findVictim**: It receives the `pid` and the `vaddr` of the new entry that will replace an existing one in the Page Table. The function uses a second chance algorithm with a circular buffer, implemented using a global variable `next_victim`, which is declared at the beginning of the file.
  - If a victim is found, the page is swapped out using `storeSwapFrame`.
  - Otherwise, the process will wait on the condition variable `pt_info.pt_cv` for pages to be freed by other processes.
- **addInPT**: It adds an entry in the Page Table given `pid`, `vaddr`, and the `index`. It returns the corresponding physical address.
- **removeFromPT**: It removes the frame from the Page Table given the `pid` and the `vaddr`.
- **freePages**: It's called by `sys__exit`; it frees all the pages from the Page Table associated with a given `pid`. It wakes any processes waiting for free pages.
- **freeContiguousPages**: It's called by `free_kpages`; it frees all contiguous pages starting from a given address `vaddr`. It uses the `allocSize` array to obtain the number of contiguous pages to free. It resets `KBIT=0` in the Page Table and `-1` in the `allocSize` array. It wakes any processes waiting for free pages.
- **getContiguousPages**: It's called by `alloc_kpages`; it allocates `nPages` contiguous pages in the physical memory. If there aren't enough pages, victim selection using the second chance algorithm is performed, and the victim is swapped out. It returns the starting address of the first allocated frame.
- **prepareCopyPT**: It's called by `as_copy`; it prepares the Page Table for a fork operation by setting the `SWAPBIT=1` for all entries in the Page Table with the given `pid`.
- **copyPTEntries**: It's called by `as_copy`; it copies all entries in the Page Table with the given `old` pid to  new free entries with a `new` pid. If there isn't enough space, the pages are copied directly to the swap file to avoid victim selection that slows down the operation. It uses `memmove` since it's a RAM-to-RAM copy.
- **endCopyPT**: It's called by `as_copy`; it ends a fork operation by setting the `SWAPBIT=0` for all entries in the Page Table with the given `pid`. It wakes any processes waiting for free pages.

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

- **as_copy**: It's called during a fork operation. It increases the reference count on the vnode of the ELF file and calls functions to duplicate entries in the page table and swap file for the new related pid using the following functions:
`prepareCopyPT`, `duplicateSwapPages`, `copyPTEntries` and `endCopyPT`.
- **as_destroy**: It closes the ELF file only when the reference count is `0`, otherwise it decreases the counter.
- **as_activate**: It calls `tlbInvalidate` to invalidate all entries in the TLB when a new process is running.
- **as_define_region**: It sets up a region of memory within the address space. In addition it computes the initial offset using the formula:
  
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

Finally, as anticipated in the [address space section](#addrspace), the starting virtual address may not align with a page boundary. In such cases, it is essential to take the `initial_offset` into account when accessing the first page. If the `initial_offset` is not zero, we need to zero-fill the page first (to ensure the initial offset bits are set to 0) and then begin loading data from the specified `initial_offset` within the block, rather than starting from the usual position of 0.

# SWAPFILE

In our implementation of swap management for OS161, we designed a system that uses a linked list of free frames, shared across all processes. Additionally, we maintain three arrays of linked lists (one for text segments, one for data segments, and one for stack segments), each with a size of `MAX_PROC` (the maximum number of allowed PIDs). This structure allows every process to have its own dedicated list for each segment, and once we determine which segment is being accessed by a particular virtual address, we can limit our search for the corresponding entry in the swap file to a small subset of frames rather than scanning the entire set.
The `swapFile` contains also the `kbuf` field which is the buffer used to perform I/O operations between the swap file and RAM during the duplication of swap pages for the fork operation.

```c
struct swapFile{
    struct swapPage **textPages;
    struct swapPage **dataPages;
    struct swapPage **stackPages;
    struct swapPage *freePages;
    void *kbuf;
    struct vnode *v; //vnode swapfile
    int sizeSF;
};
```

To handle swapping efficiently, all insertions and removals from the linked lists occur at the head. It's important to manage the precise order of these operations to avoid issues related to concurrency. 

A key aspect of our system is the similarity between the load and store operations for swapping pages and those used for handling ELF files. The main difference comes into play when we attempt to load a page that is currently in the process of being stored. In this case, the system ensures that the load operation waits until the store is complete, thus preventing I/O conflicts. To achieve this, each `swapPage` (the structure representing a page in the swap file) contains a `isStoreOp` flag, a condition variable, and a lock to synchronize access.

```c
int loadSwapFrame(vaddr_t vaddr, pid_t pid, paddr_t paddr){
    ...
    lock_acquire(listPages->operationLock);
    while(listPages->isStoreOp){                
        cv_wait(listPages->operationCV,listPages->operationLock);
    }
    lock_release(listPages->operationLock);
    ...
}
int storeSwapFrame(vaddr_t vaddr, pid_t pid, paddr_t paddr){
    ...
    lock_acquire(free_frame->operationLock);
    cv_broadcast(free_frame->operationCV, free_frame->operationLock);
    lock_release(free_frame->operationLock);
    ...
}
```

We also handle process forking by duplicating all the swap pages associated with the old PID and assigning them to the new PID in the function `duplicateSwapPages`. When a process terminates, we take all the swapPage entries from its segment lists and return them to the shared free list.
However the pages in the free list may have randomly ordered offset values, depending on how the program executed. These offsets can slow down I/O operations since higher offsets generally introduce more overhead. To mitigate this, we reorder the offset values after the program finishes. This reordering ensures consistent I/O performance for subsequent processes, especially when running multiple programs in sequence.

```c
void optimizeSwapfile(void){
    struct swapPage *p=sf->freePages;
    for(int i=0; i<sf->sizeSF; i++){
        p->swapOffset=i*PAGE_SIZE;
        p=p->next;
    }
}
```

# Statistics

The following statistics have been collected  throghuout the excecution of the programs:

1. **TLB Faults: -** (`tlb_faults`)
    -  The number of TLB misses that have occurred (not including faults that cause a program to crash).
2. **TLB Faults with Free: -** (`tlb_free_faults`)
    - TThe number of TLB misses for which there was free space in the TLB to add the new TLB entry (i.e., no replacement is required). 
3. **TLB Faults with Replace:  -** (`tlb_replace_faults`)
    -  The number of TLB misses for which there was no free space for the 
new TLB entry, so replacement was required.
4. **TLB Invalidations: -**  (`tlb_invalidations`)
    - The number of times the TLB was invalidated (this counts the number 
times the entire TLB is invalidated NOT the number of TLB entries invalidated).
5. **TLB Reloads** (`tlb_reloads`)
    - The number of TLB misses for pages that were already in memory.
6. **Page Faults (Zeroed): -** - (`pt_zeroed_faults`)
    - The number of TLB misses that required a new page to be zero
filled.
7. **Page Faults (Disk): -**  - (`pt_disk_faults`)
    - The number of TLB misses that required a page to be loaded from disk.
8. **Page Faults From ELF** - (`pt_elf_faults`)
    -  The number of page faults that require getting a page from the ELF file. 
9. **Page Faults from Swapfile: -**  - (`pt_swapfile_faults`)
    - The number of page faults that require getting a page from the swap 
file. 
10. **Swapfile Writes: -**  - (`swap_writes`)
    - The number of page faults that require writing a page to the swap file.

## Constraints

To check if the statistics have been collected correctly, Some constraints have to be respected.

- **Constraint 1: -** TLB Faults with Free + TLB Faults with Replace = TLB Faults
- **Constraint 2: -** TLB Reloads + “Page Faults (Disk) + Page Faults (Zeroed) = TLB Faults
- **Constraint 3: -** Page Faults from ELF + Page Faults from Swapfile = Page Faults (Disk)


### Main functions

- **incrementStatistics**: This function increments the appropriate TLB or page table statistic based on the type parameter, which corresponds to predefined macros.
It acquires and releases spinlocks for the statistics_tlb and statistics_pt structures to ensure only one writing at a time.

```c
#define FAULT 0
#define FAULT_WITH_FREE 1
#define FAULT_WITH_REPLACE 2
#define INVALIDATION 3
#define RELOAD 4
#define FAULT_ZEROED 5
#define FAULT_DISK 6
#define FAULT_FROM_ELF 7
#define FAULT_FROM_SWAPFILE 8
#define SWAPFILE_WRITES 9

void incrementStatistics(int type) {
    spinlock_acquire(&statistics_tlb.lock);
    spinlock_acquire(&statistics_pt.lock);

    switch (type) {
        case FAULT:
            statistics_tlb.tlb_faults++;
            break;
        case FAULT_WITH_FREE:
            statistics_tlb.tlb_faults_with_free++;
            break;
        case FAULT_WITH_REPLACE:
            statistics_tlb.tlb_faults_with_replace++;
            break;
        case INVALIDATION:
            statistics_tlb.tlb_invalidations++;
            break;
        case RELOAD:
            statistics_tlb.tlb_reloads++;
            break;
        case FAULT_ZEROED:
            statistics_pt.pt_faults_zeroed++;
            break;
        case FAULT_DISK:
            statistics_pt.pt_faults_disk++;
            break;
        case FAULT_FROM_ELF:
            statistics_pt.pt_faults_from_elf++;
            break;
        case FAULT_FROM_SWAPFILE:
            statistics_pt.pt_faults_from_swapfile++;
            break;
        case SWAPFILE_WRITES:
            statistics_pt.pt_swapfile_writes++;
            break;
        default:
            break;
    }

    spinlock_release(&statistics_pt.lock);
    spinlock_release(&statistics_tlb.lock);
}
```

- **returnSWStatistics**: This function returns the value of a TLB statistic based on the type parameter, which corresponds to predefined macros.
It acquires and releases spinlocks for the statistics_tlb and statistics_pt structures to ensure only one reading at a time.

```c
uint32_t returnTLBStatistics(int type) {
    uint32_t result;

    spinlock_acquire(&statistics_tlb.lock);

    switch (type) {
        case FAULT:
            result = statistics_tlb.tlb_faults;
            break;
        case FAULT_WITH_FREE:
            result = statistics_tlb.tlb_faults_with_free;
            break;
        case FAULT_WITH_REPLACE:
            result = statistics_tlb.tlb_faults_with_replace;
            break;
        case INVALIDATION:
            result = statistics_tlb.tlb_invalidations;
            break;
        case RELOAD:
            result = statistics_tlb.tlb_reloads;
            break;
        default:
            result = 0;
            break;
    }

    spinlock_release(&statistics_tlb.lock);
    return result;
}
```

- **constraintsCheck**: We implemented a function that automatically checks wheter the constraints have been respected. 
In case of incorrectness, the user is allerted with a kprintf on the terminal.This function checks if the constraints between TLB and page fault statistics are respected.
If a constraint is violated, a warning message is printed; otherwise, a success message is shown.

```c
void constraintsCheck(uint32_t tlbFaults, uint32_t tlbFree, uint32_t tlbReplace, uint32_t tlbReload, uint32_t disk, uint32_t zeroed, uint32_t elf, uint32_t swapfile) {
    if (tlbFaults == (tlbFree + tlbReplace)) {
        kprintf("CORRECT: the sum of TLB Faults with Free and TLB Faults with Replace is equal to TLB Faults\n");
    } else {
        kprintf("WARNING: the sum of TLB Faults with Free and TLB Faults with Replace is not equal to TLB Faults\n");
    }

    if (tlbFaults == (tlbReload + disk + zeroed)) {
        kprintf("CORRECT: the sum of TLB Reloads, Page Faults Disk and Page Faults Zeroed is equal to TLB Faults\n");
    } else {
        kprintf("WARNING: the sum of TLB reloads, Page Faults Disk and Page Faults Zeroed is not equal to TLB Faults\n");      
    }

    if (disk == (elf + swapfile)) {
        kprintf("CORRECT: the sum of Page Faults from ELF and Page Faults from Swapfile is equal to Page Faults (Disk)\n\n");
    } else {
        kprintf("WARNING: the sum of Page Faults from ELF and Page Faults from Swapfile is not equal to Page Faults (Disk)\n\n");      
    }
}
```












