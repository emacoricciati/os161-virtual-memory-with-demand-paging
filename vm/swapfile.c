#include "swapfile.h"

#define MAX_SIZE 9*1024*1024 //Size of the swap file (9 MB)

struct swapFile *sf;

#if OPT_DEBUG
void printPageLists(pid_t pid){

    struct swapPage *i;

    kprintf("*** SWAP PAGE LIST FOR PROCESS %d: ***\n",pid);
    kprintf("Text:\n");
    for(i=sf->textPages[pid];i!=NULL;i=i->next){
        kprintf("addr: 0x%x, offset: 0x%x, next: 0x%x\n",i->vaddr,i->swapOffset,(unsigned int)i->next);
    }
    kprintf("Data:\n");
    for(i=sf->dataPages[pid];i!=NULL;i=i->next){
        kprintf("addr: 0x%x, offset: 0x%x, next: 0x%x\n",i->vaddr,i->swapOffset,(unsigned int)i->next);
    }
    kprintf("Stack:\n");
    for(i=sf->stackPages[pid];i!=NULL;i=i->next){
        kprintf("addr: 0x%x, offset: 0x%x, next: 0x%x\n",i->vaddr,i->swapOffset,(unsigned int)i->next);
    }
    kprintf("\n");

}
#endif

int loadSwapFrame(vaddr_t vaddr, pid_t pid, paddr_t paddr){
    int result;
    struct iovec iov;
    struct uio ku;

    //Asserting if the pid is the same of the one of the current process
    KASSERT(pid==curproc->p_pid);

    struct addrspace *as=proc_getas();
    struct swapPage *listPages=NULL, *prevPages=NULL;
    int segVar=-1;     //It's used for removal from the head but can also be used for debugging

    //1: Identify the right segment afterward the list will point to the correct entry
    if(vaddr>=as->as_vbase1 && vaddr <= as->as_vbase1 + as->as_npages1 * PAGE_SIZE ){
        listPages = sf->textPages[pid];
        segVar=0;
    }

    if(vaddr>=as->as_vbase2 && vaddr <= as->as_vbase2 + as->as_npages2 * PAGE_SIZE ){
        listPages = sf->dataPages[pid];
        segVar=1;
    }

    if(vaddr <= USERSTACK && vaddr>as->as_vbase2 + as->as_npages2 * PAGE_SIZE){
        listPages = sf->stackPages[pid];
        segVar=2;
    }

    //Managing the case in which the segment is not found
    if(segVar==-1){
        panic("Wrong virtual address for load: 0x%x, process=%d\n",vaddr,curproc->p_pid);
    }


    //2: Search for the right entry in the list
    while(listPages!=NULL){
        if(listPages->vaddr==vaddr){ //Entry found

            //As a consequence of parallelism we have to follow a specific order in the operations
            //-1: RRemove the entry frmo the process list, otherwise the old entry could be considered valid
            //-2: I/0 Operation, but with the exception that the entry can't be placed in the free list
            //-3: Place the entry inside the free list (so after the I/O operation has been completed)

            if(prevPages!=NULL){ //If List is not the first entry
                KASSERT(prevPages->next==listPages);
                DEBUG(DB_VM,"We removed 0x%x from process %d, so now 0x%x points to 0x%x\n",vaddr,pid,prevPages!=NULL?prevPages->vaddr:(unsigned int)NULL,prevPages->next->vaddr);
                prevPages->next=listPages->next;      //remove list from the process list
            }
            else{                       //Removing from head
                if(segVar==0){
                    sf->textPages[pid]=listPages->next;
                }
                if(segVar==1){
                    sf->dataPages[pid]=listPages->next;
                }
                if(segVar==2){
                    sf->stackPages[pid]=listPages->next;
                }
            }

            lock_acquire(listPages->operationLock);
            while(listPages->isStoreOp){                 //we have to wait until the entry is not stored
                cv_wait(listPages->operationCV,listPages->operationLock);         //waiting on the condition variable of the entry
            }
            lock_release(listPages->operationLock);
            
            //DEBUG(DB_VM,"LOAD SWAP in 0x%x (virtual: 0x%x) for process %d\n",list->offset, vaddr, pid);

            incrementStatistics(FAULT_DISK);               //Update of the statistics  

            uio_kinit(&iov,&ku,(void*)PADDR_TO_KVADDR(paddr),PAGE_SIZE,listPages->swapOffset,UIO_READ);          //paddr is the physical address of the frame and it's used in order toa void faults

            result = VOP_READ(sf->v,&ku);                 //read
            if(result){
                panic("VOP_READ in swapfile failed, with result=%d",result);
            }
            //DEBUG(DB_VM,"ENDED LOAD SWAP in 0x%x (virtual: 0x%x) for process %d\n",list->offset, list->vaddr, pid);

            listPages->next=sf->freePages;                      //we put the entry in the free list
            sf->freePages=listPages;

            incrementStatistics(FAULT_FROM_SWAPFILE);               //Update of the statistics

            listPages->vaddr=0;                              //reset the virtual address in the list

            #if OPT_DEBUG
            printPageLists(pid);                           //print the list (updated)
            #endif

            return 1;                                  //entry found in the swapfile, so return 1
        }
        
        prevPages=listPages;                                     //we update prev and list
        listPages=listPages->next;
        KASSERT(prevPages->next==listPages);
    }

    return 0;                                               //no entry found, so return 0

}

int storeSwapFrame(vaddr_t vaddr, pid_t pid, paddr_t paddr){

    
    int result;
    struct iovec iov; //I/O vector structure for scatter/gather I/O
    struct uio ku;    //UIO structure for kernel I/O operations

    struct addrspace *as = proc_getas(); //current process's address space
    struct swapPage *free_frame;        //pointer to a free swap frame
    int valid_address = 0;               //flag to check if the address is valid

    /**
     * Due to parallelism, we must ensure the correct order of operations:
     * 1. Acquire a free frame from the free list.
     *    - Do not insert it into the process's swap list before the I/O operation completes.
     *    - This prevents inconsistencies where the process might attempt to access the swap entry prematurely.
     * 2. During the store operation, the page cannot be accessed as it contains invalid data.
     *    - Use the `store` flag to indicate an ongoing store operation for the frame.
    */

    free_frame = sf->freePages;    //first free frame from the swap's free list

    if (free_frame == NULL){
        panic("The swapfile is full!"); //no free frame is available -> the swapfile is full
    }

    KASSERT(free_frame->isStoreOp == 0); //check if the frame is being stored
    sf->freePages = free_frame->next; //remove the frame from the free list

    // Determine which segment the virtual address belongs to and insert the frame accordingly

    //check if the address is in the text segment
    if (vaddr >= as->as_vbase1 && vaddr <= as->as_vbase1 + as->as_npages1 * PAGE_SIZE ){
        free_frame->next = sf->textPages[pid]; // Insert at the head of the text swap list for the process
        sf->textPages[pid] = free_frame;
        valid_address = 1;
    }

    // check if the address is in the data segment
    if (vaddr >= as->as_vbase2 && vaddr <= as->as_vbase2 + as->as_npages2 * PAGE_SIZE ){
        free_frame->next = sf->dataPages[pid]; // Insert at the head of the data swap list for the process
        sf->dataPages[pid] = free_frame;
        valid_address = 1;
    }

    //check if the address is in the stack segment
    if (vaddr <= USERSTACK && vaddr > as->as_vbase2 + as->as_npages2 * PAGE_SIZE){
        free_frame->next = sf->stackPages[pid]; // Insert at the head of the stack swap list for the process
        sf->stackPages[pid] = free_frame;
        valid_address = 1;
    }

    if (valid_address == 0){
        panic("Wrong vaddr for store: 0x%x\n", vaddr); //the address doesn't belong to any segment
    }

    free_frame->vaddr = vaddr; //assign the virtual address to the swap frame

    DEBUG(DB_VM, "STORE SWAP in 0x%x (virtual: 0x%x) for process %d\n", free_frame->swapOffset, free_frame->vaddr, pid);
    
    free_frame->isStoreOp = 1; //the frame is being stored

    // Initialize the UIO structure for writing to the swap file
    uio_kinit(&iov, &ku, (void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE, free_frame->swapOffset, UIO_WRITE);
    
    result = VOP_WRITE(sf->v, &ku); // Perform the write operation to the swap file
    if(result){
        panic("VOP_WRITE in swapfile failed, with result=%d", result); //write failure
    }

    free_frame->isStoreOp = 0; //storing ended

    // Synchronize with any processes waiting on this swap frame
    lock_acquire(free_frame->operationLock);
    cv_broadcast(free_frame->operationCV, free_frame->operationLock);
    lock_release(free_frame->operationLock);

    DEBUG(DB_VM, "ENDED STORE SWAP in 0x%x (virtual: 0x%x) for process %d\n", free_frame->swapOffset, free_frame->vaddr, pid);
    DEBUG(DB_VM, "We added 0x%x to process %d, that points to 0x%x\n", vaddr, pid, free_frame->next ? free_frame->next->vaddr : 0x0);

    incrementStatistics(SWAPFILE_WRITES); 
    return 1; 
        
}

void freeProcessPagesInSwap(pid_t pid){
    struct swapPage *elem, *next;

    #if OPT_DEBUG
    int r = 0;
    #endif

    //We iterate on text, data and stack lists because we have to remove all the elements that belong to the ended process

    if(sf->textPages[pid]!=NULL){
        for(elem=sf->textPages[pid];elem!=NULL;elem=next){
            lock_acquire(elem->operationLock);
            while(elem->isStoreOp){                                 //we have to wait if there's a isStoreOp operation on the element
                cv_wait(elem->operationCV,elem->operationLock);
            }
            lock_release(elem->operationLock);

            next=elem->next;                                    //We save next to correctly initialize elem in the following iteration
            elem->next=sf->freePages;
            sf->freePages=elem;
        }
        sf->textPages[pid]=NULL;
    }


    //COMMENTA TU QUI
    if(sf->dataPages[pid]!=NULL){
        #if OPT_DEBUG
        if(r==0){                                             //if r is equal to 0 we remove thre process from swap
            DEBUG(DB_VM,"FIRST REMOVE PROCESS FROM SWAP\n");
            r++;
        }
        #endif
        for(elem=sf->dataPages[pid];elem!=NULL;elem=next){
            lock_acquire(elem->operationLock);
            while(elem->isStoreOp){
                cv_wait(elem->operationCV,elem->operationLock);
            }
            lock_release(elem->operationLock);

            next=elem->next;
            elem->next=sf->freePages;
            sf->freePages=elem;
        }
        sf->dataPages[pid]=NULL;
    }

    if(sf->stackPages[pid]!=NULL){
        #if OPT_DEBUG
        if(r==0){
            DEBUG(DB_VM,"FIRST REMOVE PROCESS FROM SWAP\n");
            r++;
        }
        #endif
        for(elem=sf->stackPages[pid];elem!=NULL;elem=next){
            lock_acquire(elem->operationLock);
            while(elem->isStoreOp){
                cv_wait(elem->operationCV,elem->operationLock);
            }
            lock_release(elem->operationLock);

            next=elem->next;
            elem->next=sf->freePages;
            sf->freePages=elem;
        }
        sf->stackPages[pid]=NULL;
    }
}

void duplicateSwapPages(pid_t new_pid, pid_t old_pid) {
    // Log the start of the fork operation
    DEBUG(DB_VM,"Process %d performs a kmalloc to fork %d\n",curproc->p_pid,new_pid);

    struct uio u;                //user I/O structure for I/O operations
    struct iovec iov;            //I/O vector structure for scatter/gather I/O
    int result;                  //result of I/O

    struct swapPage *ptr, *free = NULL;  //pointers for traversing and allocating swap cells

    // Duplicating the text segment swap entries for the new process
    if (sf->textPages[old_pid] != NULL) {
        for (ptr = sf->textPages[old_pid]; ptr != NULL; ptr = ptr->next) {
            
            // Fetch a free swap cell from the free list
            free = sf->freePages;
            if (free == NULL) {
                panic("The swapfile is full!");  //no free swap cells available
            }

            sf->freePages = free->next;              //update the free list
            free->next = sf->textPages[new_pid];     //insert the free cell into the new process's text segment list
            sf->textPages[new_pid] = free;

            KASSERT(!free->isStoreOp);  //cell not in storing operation at the moment

            //wait for storing operations to end
            lock_acquire(ptr->operationLock);
            while (ptr->isStoreOp) {
                cv_wait(ptr->operationCV, ptr->operationLock);
            }
            lock_release(ptr->operationLock);

            DEBUG(DB_VM,"Copying from 0x%x to 0x%x\n",ptr->swapOffset,free->swapOffset);

            // read the page from the old process's swap entry into the kernel buffer
            uio_kinit(&iov, &u, sf->kbuf, PAGE_SIZE, ptr->swapOffset, UIO_READ);
            result = VOP_READ(sf->v, &u);
            if (result) {
                panic("VOP_READ in swapfile failed, with result=%d", result);  //read failure
            }

            // Write the page from the kernel buffer into the new process's swap entry
            uio_kinit(&iov, &u, sf->kbuf, PAGE_SIZE, free->swapOffset, UIO_WRITE);
            result = VOP_WRITE(sf->v, &u);
            if (result) {
                panic("VOP_WRITE in swapfile failed, with result=%d", result);  //write failure
            }

            DEBUG(DB_VM,"Copied text from 0x%x to 0x%x for process %d\n",ptr->vaddr,free->vaddr,new_pid);
            free->vaddr = ptr->vaddr; //set virtual address for the new swap entry
        }
    }
    
    // Duplicating the data segment swap entries for the new process
    if (sf->dataPages[old_pid] != NULL) {
        for (ptr = sf->dataPages[old_pid]; ptr != NULL; ptr = ptr->next) {
            
            // Fetch a free swap cell from the free list
            free = sf->freePages;
            if (free == NULL) {
                panic("The swapfile is full!");  //no free swap cells available
            }

            sf->freePages = free->next;              //update the free list
            free->next = sf->dataPages[new_pid];     //insert the free cell into the new process's data segment list
            sf->dataPages[new_pid] = free;

            KASSERT(!free->isStoreOp);  //cell not in storing operation at the moment

           //wait for storing operations to end
            lock_acquire(ptr->operationLock);
            while (ptr->isStoreOp) {
                cv_wait(ptr->operationCV, ptr->operationLock);
            }
            lock_release(ptr->operationLock);

            DEBUG(DB_VM,"Copying from 0x%x to 0x%x\n",ptr->swapOffset,free->swapOffset);

            // Read the page from the old process's swap entry into the kernel buffer
            uio_kinit(&iov, &u, sf->kbuf, PAGE_SIZE, ptr->swapOffset, UIO_READ);
            result = VOP_READ(sf->v, &u);
            if (result) {
                panic("VOP_READ in swapfile failed, with result=%d", result);  //read failure
            }

            // Write the page from the kernel buffer into the new process's swap entry
            uio_kinit(&iov, &u, sf->kbuf, PAGE_SIZE, free->swapOffset, UIO_WRITE);
            result = VOP_WRITE(sf->v, &u);
            if (result) {
                panic("VOP_WRITE in swapfile failed, with result=%d", result);  //write failure
            }

            DEBUG(DB_VM,"Copied data from 0x%x to 0x%x for process %d\n",ptr->vaddr,free->vaddr,new_pid);
            free->vaddr = ptr->vaddr; //set virtual address for the new swap entry
        }
    }

    // Duplicating the stack segment swap entries for the new process
    if (sf->stackPages[old_pid] != NULL) {
        for (ptr = sf->stackPages[old_pid]; ptr != NULL; ptr = ptr->next) {
            
            // Fetch a free swap cell from the free list
            free = sf->freePages;
            if (free == NULL) {
                panic("The swapfile is full!");  //no free swap cells available
            }

            sf->freePages = free->next;              //update the free list
            free->next = sf->stackPages[new_pid];    //insert the free cell into the new process's stack segment list
            sf->stackPages[new_pid] = free;

            KASSERT(!free->isStoreOp); //cell not in storing operation at the moment

            //wait for storing operations to end
            lock_acquire(ptr->operationLock);
            while (ptr->isStoreOp) {
                cv_wait(ptr->operationCV, ptr->operationLock);
            }
            lock_release(ptr->operationLock);

            DEBUG(DB_VM,"Copying from 0x%x to 0x%x\n",ptr->swapOffset,free->swapOffset);

            // Read the page from the old process's swap entry into the kernel buffer
            uio_kinit(&iov, &u, sf->kbuf, PAGE_SIZE, ptr->swapOffset, UIO_READ);
            result = VOP_READ(sf->v, &u);
            if (result) {
                panic("VOP_READ in swapfile failed, with result=%d", result);  //read failure
            }

            // Write the page from the kernel buffer into the new process's swap entry
            uio_kinit(&iov, &u, sf->kbuf, PAGE_SIZE, free->swapOffset, UIO_WRITE);
            result = VOP_WRITE(sf->v, &u);
            if (result) {
                panic("VOP_WRITE in swapfile failed, with result=%d", result);  //write failure
            }

            DEBUG(DB_VM,"Copied stack from 0x%x to 0x%x for process %d\n",ptr->vaddr,free->vaddr,new_pid);
            free->vaddr = ptr->vaddr; //set virtual address for the new swap entry
        }
    }
    
}

void optimizeSwapfile(void){

    struct swapPage *p=sf->freePages;

    // Start from index i=0, ensuring that the first free frame has an offset of 0
    for(int i=0; i<sf->sizeSF; i++){
        p->swapOffset=i*PAGE_SIZE;
        p=p->next;
    }
}