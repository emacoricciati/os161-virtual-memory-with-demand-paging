#include "pt.h"
#include "spinlock.h"
#include "vm.h"
#include "mainbus.h"
#include "lib.h"
#include "current.h"
#include "proc.h"
#include "segments.h"

int next_victim = 0; // second chance


static int findFreeEntryPT(){
    for(int i=0; i<pt_info.ptSize; i++){
        if(GET_VALBIT(pt_info.pt[i].ctl)==0 &&  GET_KBIT(pt_info.pt[i].ctl) == 0 && GET_SWAPBIT(pt_info.pt[i].ctl) == 0 && GET_IOBIT(pt_info.pt[i].ctl) == 0){
            return i;
        }
    }
    return -1;
}

void initPT(void){
    struct spinlock stealmem_lock;
    spinlock_init(&stealmem_lock);
    spinlock_acquire(&stealmem_lock);
    int nFrames;
    nFrames = (mainbus_ramsize() - ram_stealmem(0))/PAGE_SIZE;    //number of frames in physical memory (= IPT size)
    spinlock_release(&stealmem_lock);

    pt_info.pt = kmalloc(sizeof(struct pt_entry_s) * nFrames);   //one entry for each available frame
    
    spinlock_acquire(&stealmem_lock);
    if (pt_info.pt == NULL){
        panic("Error on allocating the Inverted Page Table");
    }

    pt_info.pt_lock = lock_create("pagetable-lock");
    if(pt_info.pt_lock==NULL){
        panic("Error. Lock hasn't been initialized");
    }

    pt_info.pt_cv = cv_create("pagetable-cv");
    if(pt_info.pt_lock==NULL){
        panic("Error. The Condition Variable hasn't been initialized");
    }
    spinlock_release(&stealmem_lock);
    
    pt_info.allocSize = kmalloc(sizeof(int) * nFrames);

    spinlock_acquire(&stealmem_lock);
    if (pt_info.allocSize == NULL){
        panic("Error. allocSize not allocated");
    }
    for (int i = 0; i < nFrames; i++) {
        pt_info.pt[i].ctl = 0;
        pt_info.allocSize[i]=-1;
    }

    DEBUG(DB_IPT,"RAM INFO:\n\tSize :0x%x\n\tFirst free physical address: 0x%x\n\tAvailable memory: 0x%x\n\n",mainbus_ramsize(),ram_stealmem(0),mainbus_ramsize()-ram_stealmem(0));

    pt_info.firstfreepaddr = ram_stealmem(0); //ram_stealmem(0) returns the first free physical address (=from where our IPT starts)
    pt_info.ptSize = ((mainbus_ramsize() - ram_stealmem(0)) / PAGE_SIZE) - 1; // -1 because the first frame is used for the IPT  
    
    pt_active=1; //IPT ready
    spinlock_release(&stealmem_lock);
}

int getPAddressPT(vaddr_t v_addr, pid_t pid){   //page address, process pid
    int i = getIndexFromPT(v_addr, pid);       //Search for the entry in PT

    if(i==-1){
        return i;                       //Entry not found-->return -1
    }

    KASSERT(pt_info.pt[i].vPage==v_addr);
    KASSERT(pt_info.pt[i].pid==pid);
    KASSERT(GET_IOBIT(pt_info.pt[i].ctl)==0);
    KASSERT(GET_KBIT(pt_info.pt[i].ctl)==0);
    KASSERT(GET_TLBBIT(pt_info.pt[i].ctl)==0);

    pt_info.pt[i].ctl = SET_TLBBITONE(pt_info.pt[i].ctl); // entry will be in TLB

    return i * PAGE_SIZE + pt_info.firstfreepaddr; // send the paddr found
}

paddr_t getFramePT(vaddr_t v_addr){
    // wrapper function to get the physical address
    pid_t current_pid = curproc->p_pid;
    int val = getPAddressPT(v_addr,current_pid);
    paddr_t p_addr;
    if(val != -1){
        // virtual address is available in the pt
        incrementStatistics(RELOAD);
        p_addr = (paddr_t) (val);
        return p_addr;
    }

    DEBUG(DB_IPT,"PID=%d wants to load 0x%x\n",current_pid,v_addr);
    // virtual address is not available in the page table, searching free entry in the pt
    int entry = findFreeEntryPT();
    if(entry != -1){
        // free entry available
        KASSERT(entry < pt_info.ptSize);
        pt_info.pt[entry].ctl=SET_VALBITONE(pt_info.pt[entry].ctl);
        pt_info.pt[entry].ctl=SET_IOBITONE(pt_info.pt[entry].ctl);
        p_addr = addInPT(v_addr, current_pid, entry);
    }
    else {
        // free entry not available in the pt, find a victim
        entry = findVictim(v_addr, current_pid);
        KASSERT(entry < pt_info.ptSize);
        p_addr = pt_info.firstfreepaddr + entry * PAGE_SIZE;
    }

    loadPage(v_addr,current_pid,p_addr);
    pt_info.pt[entry].ctl = SET_IOBITZERO(pt_info.pt[entry].ctl); // end of I/O operation
    pt_info.pt[entry].ctl = SET_TLBBITONE(pt_info.pt[entry].ctl); // entry will be in TLB

    return p_addr;
}

int findVictim(vaddr_t v_addr, pid_t pid){

    // circular buffer implementation
    int i, end = next_victim, n = 0, old_vdty = 0; 
    vaddr_t old_vaddr;
    pid_t old_pid;

    for(i = next_victim;; i = (i+1)%pt_info.ptSize){
        // if the page can be swapped out
        if(GET_KBIT(pt_info.pt[i].ctl) == 0 && GET_TLBBIT(pt_info.pt[i].ctl) == 0 && GET_SWAPBIT(pt_info.pt[i].ctl) == 0 && GET_IOBIT(pt_info.pt[i].ctl) == 0){ 
            // second change algorithm
            if(GET_REFBIT(pt_info.pt[i].ctl) == 0){ // no check on validity, if validity bit = 0 it means that there is a free entry
                // victim is found
                KASSERT(GET_KBIT(pt_info.pt[i].ctl)==0); // no kmalloc
                KASSERT(GET_VALBIT(pt_info.pt[i].ctl)); // valid entry
                KASSERT(GET_SWAPBIT(pt_info.pt[i].ctl)==0); // not in fork operation
                KASSERT(GET_IOBIT(pt_info.pt[i].ctl)==0); // no I/O operation on swap file
                KASSERT(GET_TLBBIT(pt_info.pt[i].ctl)==0); // not in TLB

                /** To address synchronization problems, all new values must be 
                 * assigned prior to performing load/store operations, specifically 
                 * before entering a sleep state.
                 * The old values are stored in temporary variables to be used later in the swap operation.
                 * */

                old_pid = pt_info.pt[i].pid;
                old_vaddr = pt_info.pt[i].vPage;
                old_vdty = GET_VALBIT(pt_info.pt[i].ctl);  
                pt_info.pt[i].ctl = 0; // clear bits
                addInPT(v_addr, pid, i); // add and replace the old entry
                pt_info.pt[i].ctl = SET_IOBITONE(pt_info.pt[i].ctl); // start I/O operation
                pt_info.pt[i].ctl = SET_VALBITONE(pt_info.pt[i].ctl); // now the entry is valid
                if(old_vdty){
                    // if the page was valid, store it in the swap file
                    storeSwapFrame(old_vaddr, old_pid, pt_info.firstfreepaddr + i*PAGE_SIZE);  //swap 
                }
                next_victim = (i+1) % pt_info.ptSize; // next victim for next time
                return i;
            }
            else {
                pt_info.pt[i].ctl = SET_REFBITZERO(pt_info.pt[i].ctl);
            }
        }
        if((i+1)%pt_info.ptSize == end){
            // victim is not found, iterate again (all ref bits setted to one)
            if(n < 2){
                n++;
                continue;
            }
            else {
                // victim is still not found, let's wait for freed pages by other processes
                lock_acquire(pt_info.pt_lock);
                cv_wait(pt_info.pt_cv, pt_info.pt_lock);
                lock_release(pt_info.pt_lock);
                n = 0; // start 
            }
        }
    }

    panic("Victim not found!");

}

void freePages(pid_t pid){  // frees all pages from PT using pid

    for (int i = 0; i < pt_info.ptSize; i++){
        if (pt_info.pt[i].pid == pid){
            if(GET_VALBIT(pt_info.pt[i].ctl) && GET_KBIT(pt_info.pt[i].ctl)==0){ //We don't free: kmalloc pages                                   
                KASSERT(GET_VALBIT(pt_info.pt[i].ctl));
                KASSERT(GET_KBIT(pt_info.pt[i].ctl)==0);
                KASSERT(GET_SWAPBIT(pt_info.pt[i].ctl)==0);
                KASSERT(GET_IOBIT(pt_info.pt[i].ctl)==0);
                removeFromPT(pt_info.pt[i].vPage, pt_info.pt[i].pid);
            }
        } 
    }

    lock_acquire(pt_info.pt_lock);
    cv_broadcast(pt_info.pt_cv,pt_info.pt_lock); //waking up processes wating for free pages
    lock_release(pt_info.pt_lock);

}

void freeContiguousPages(vaddr_t addr){
    int i, index, n_contig_pages;

    index = (KVADDR_TO_PADDR(addr) - pt_info.firstfreepaddr) / PAGE_SIZE;  //get the index in the IPT 
    n_contig_pages = pt_info.allocSize[index];        //get the number of contiguous allocated pages

   
    for(i=index;i<index+n_contig_pages;i++){
        KASSERT(GET_KBIT(pt_info.pt[i].ctl));                       //page assigned with kmalloc
        pt_info.pt[i].ctl = SET_VALBITZERO(pt_info.pt[i].ctl);      //valid bit = 0 -> page not valid anymore
        pt_info.pt[i].ctl = SET_KBITZERO(pt_info.pt[i].ctl);        //clear kmalloc bit                                               
    }

    pt_info.allocSize[index]=-1;                              //clear the number of contiguous allocated pages

    
    if(curthread->t_in_interrupt == false){                     //if I'm in the interrupt i cannot acquire a lock
        lock_acquire(pt_info.pt_lock);
        cv_broadcast(pt_info.pt_cv,pt_info.pt_lock);            //Since we freed some pages, we wake up the processes waiting on the cv.
        lock_release(pt_info.pt_lock);
    }
    else{
        cv_broadcast(pt_info.pt_cv,pt_info.pt_lock);
    }
    

}

int getIndexFromPT(vaddr_t vad, pid_t pid){  
    for (int i = 0; i < pt_info.ptSize; i++){
        if (pt_info.pt[i].pid == pid && pt_info.pt[i].vPage == vad){
            if(GET_KBIT(pt_info.pt[i].ctl)==0){
                return i;
            }
        } 
    }
    return -1; //not found
}

void removeFromPT(vaddr_t vad, pid_t pid) {
    int i=getIndexFromPT(vad, pid);
    if(i==-1){
        kprintf("Page not found\n");
        return;
    }else{
        pt_info.pt[i].vPage=0;   
        pt_info.pt[i].pid=0;
        pt_info.pt[i].ctl=0;
    }
    return;
}

paddr_t addInPT(vaddr_t v_addr, pid_t pid, int index){
    KASSERT(v_addr!=0);                            
    KASSERT(pid!=0);

    pt_info.pt[index].vPage=v_addr;
    pt_info.pt[index].pid=pid;
    return (paddr_t) (pt_info.firstfreepaddr + index*PAGE_SIZE);
}

static int checkEntryValidity(uint8_t ctl){
    if(GET_TLBBIT(ctl) != 0){
        return 1;
    }
    if(GET_VALBIT(ctl)!=0){
        return 1;
    }
    if(GET_KBIT(ctl) != 0){
        return 1;
    }
    if(GET_IOBIT(ctl)!=0){
        return 1;
    }
    if(GET_SWAPBIT(ctl)!=0){
        return 1;
    }
    return 0;
}


paddr_t getContiguousPages(int nPages){
    int i, j, first=-1, prev=0, old_vdty;
    int firstIteration=0;
    pid_t old_pid;
    vaddr_t old_vaddr;

    DEBUG(DB_IPT,"Process %d performs kmalloc for %d pages\n", curproc->p_pid,nPages);

    if (nPages > pt_info.ptSize){
        panic("Can't do kmalloc, not enough memory"); //Impossible allocation
    }

    //Option 1: searching for contiguos npages in order to avoid swapping
    for (i = 0; i < pt_info.ptSize; i++){
        if(i!=0){           //Checking the validity of the previous entry
            prev = checkEntryValidity(pt_info.pt[i-1].ctl);
        }
        if(GET_VALBIT(pt_info.pt[i].ctl)==0 && GET_TLBBIT(pt_info.pt[i].ctl)==0 && GET_KBIT(pt_info.pt[i].ctl)==0 && GET_IOBIT(pt_info.pt[i].ctl)==0 && GET_SWAPBIT(pt_info.pt[i].ctl)==0 && (i==0 || prev)){  //checking if the current entry is not valid while the previous one was valid (or if the first entry is not valid)
            first=i; 
        } 
        if(first>=0 && GET_VALBIT(pt_info.pt[i].ctl)==0 && GET_TLBBIT(pt_info.pt[i].ctl)==0 && GET_SWAPBIT(pt_info.pt[i].ctl)==0 && GET_IOBIT(pt_info.pt[i].ctl)==0 &&  GET_KBIT(pt_info.pt[i].ctl)==0 && i-first==nPages-1){ //We found npages contiguous entries
            //we are allocating with kmalloc
            DEBUG(DB_IPT,"Kmalloc for process %d, entry: %d\n",curproc->p_pid,first);
            for(j=first;j<=i;j++){
                KASSERT(GET_KBIT(pt_info.pt[j].vPage)==0);          
                KASSERT(GET_VALBIT(pt_info.pt[j].ctl)==0);
                KASSERT(GET_TLBBIT(pt_info.pt[j].ctl)==0);
                KASSERT(GET_IOBIT(pt_info.pt[j].ctl)==0);
                KASSERT(GET_SWAPBIT(pt_info.pt[j].ctl)==0);
                pt_info.pt[j].ctl=SET_VALBITONE(pt_info.pt[j].ctl);   //Set pages as valid
                pt_info.pt[j].ctl=SET_KBITONE(pt_info.pt[j].ctl);     //This page can't be swapped out until when we perform a free on it
            }
            pt_info.allocSize[first] = nPages; //We save in position first the number of contiguous pages allocated. It'll be useful while freeing
            return first * PAGE_SIZE + pt_info.firstfreepaddr;
        }
    }


    //Option 2: we perform victim selection because table is full
    while(1){  //Exit only when we find n contiguous victims
        for (i = next_victim; i < pt_info.ptSize; i++){
            if (GET_KBIT(pt_info.pt[i].ctl) == 0 && GET_TLBBIT(pt_info.pt[i].ctl) == 0 && GET_IOBIT(pt_info.pt[i].ctl) == 0 && GET_SWAPBIT(pt_info.pt[i].ctl) == 0){ //Checking if the entry can be removed
                if(GET_REFBIT(pt_info.pt[i].ctl) && GET_VALBIT(pt_info.pt[i].ctl)){ //If the page is valid and has reference=1 we set reference=0 (due to second chance algorithm) and we continue
                    pt_info.pt[i].ctl = SET_REFBITZERO(pt_info.pt[i].ctl);
                    continue;
                }
                int valid= checkEntryValidity(pt_info.pt[i].ctl);
                if ((GET_REFBIT(pt_info.pt[i].ctl) == 0 || GET_VALBIT(pt_info.pt[i].ctl) == 0) && (i==0 || valid)){//If the current entry can be removed and the previous is valid, i is the start of the interval
                    first = i;
                }
                if(first>=0 && (GET_REFBIT(pt_info.pt[i].ctl) == 0 || GET_VALBIT(pt_info.pt[i].ctl) == 0) && i-first==nPages-1){ //We found npages contiguous entries that can be removed
                    for(j=first;j<=i;j++){
                        KASSERT(GET_KBIT(pt_info.pt[j].ctl)==0);
                        KASSERT(GET_REFBIT(pt_info.pt[j].ctl)==0 || GET_VALBIT(pt_info.pt[j].ctl)==0);
                        KASSERT(GET_TLBBIT(pt_info.pt[j].ctl)==0);
                        KASSERT(GET_IOBIT(pt_info.pt[j].ctl)==0);
                        KASSERT(GET_SWAPBIT(pt_info.pt[j].ctl)==0);
                        old_pid = pt_info.pt[j].pid;
                        old_vaddr = pt_info.pt[j].vPage;
                        old_vdty = GET_VALBIT(pt_info.pt[j].ctl);
                        // replace entry
                        pt_info.pt[j].ctl = 0;
                        // passing 1 as vaddr to addInPT, because we don't need to store the vaddr in the pt (avoid assertion failure)
                        addInPT(1, curproc->p_pid, j);
                        pt_info.pt[j].ctl = SET_KBITONE(pt_info.pt[j].ctl); //To remember that this page can't be swapped out until when we perform a free
                        pt_info.pt[j].ctl = SET_VALBITONE(pt_info.pt[j].ctl); //Set pages as valid
                        if(old_vdty){
                            pt_info.pt[j].ctl = SET_IOBITONE(pt_info.pt[j].ctl); // start of I/O operation
                            storeSwapFrame(old_vaddr, old_pid, pt_info.firstfreepaddr + j*PAGE_SIZE);  //swap
                            pt_info.pt[j].ctl = SET_IOBITZERO(pt_info.pt[j].ctl); //end of I/O operation
                        }
                    }
                    pt_info.allocSize[first]=nPages; //in order to execute the free operations we save in position first the number of pages
                    next_victim = (i + 1) % pt_info.ptSize; //updating the lastIndex for second chance
                    return first*PAGE_SIZE + pt_info.firstfreepaddr;
                }
            }
        }
        next_victim=0; //At the end of page we restart, as pages must be physically contiguous and not contiguous in the circular buffer of the Inverted Page Table

        if(firstIteration<2){ //We perform 2 full iterations in order to have a complete execution of the second chance algorithm
            firstIteration++;
        }else{
            lock_acquire(pt_info.pt_lock);
            cv_wait(pt_info.pt_cv,pt_info.pt_lock); //If after 2 complete iterations we didn't find a suitable interval we sleep until when something changes
            lock_release(pt_info.pt_lock);
            firstIteration=0; //To perform again 2 full iterations
        }

        first=-1; //reset of variable first
    }

    return 0;
}


int tlbUpdateBit(vaddr_t v, pid_t pid)
{     

    int i;

    for (i = 0; i < pt_info.ptSize; i++)
    {
        if (pt_info.pt[i].vPage == v && pt_info.pt[i].pid==pid && GET_VALBIT(pt_info.pt[i].ctl)==1) // Page found
        {
            if(GET_TLBBIT(pt_info.pt[i].ctl) == 0){
                // error
                kprintf("Error for process %d, vaddr 0x%x, ctl=0x%x\n",pid,v,pt_info.pt[i].ctl);
            }
            KASSERT(GET_KBIT(pt_info.pt[i].vPage)==0);
            KASSERT(GET_TLBBIT(pt_info.pt[i].ctl) != 0); // it must be inside TLB
            pt_info.pt[i].ctl = SET_TLBBITZERO(pt_info.pt[i].ctl); // remove TLB bit
            pt_info.pt[i].ctl = SET_REFBITONE(pt_info.pt[i].ctl);  // set ref bit to 1

            return 1;                                    
        }
    }

    return -1;
}


void copyPTEntries(pid_t old, pid_t new){ // needed for fork

    int pos;

    // The goal is to copy all the pages associated with oldpid, but assign them to newpid instead.
    for(int i=0;i<pt_info.ptSize;i++){

        // All valid pages from old are copied, excluding the kmalloc pages.
        if(pt_info.pt[i].pid==old && GET_VALBIT(pt_info.pt[i].ctl)!=0 && GET_KBIT(pt_info.pt[i].ctl)==0){ 
            pos = findFreeEntryPT();
            // If there is no available free space, the page is copied directly to the swap file to avoid victim selection, which may be impractical if space is insufficient.
            if(pos==-1){
                KASSERT(GET_IOBIT(pt_info.pt[i].ctl) == 0);
                KASSERT(GET_SWAPBIT(pt_info.pt[i].ctl) != 0);
                KASSERT(GET_KBIT(pt_info.pt[i].ctl) == 0);
                DEBUG(DB_IPT,"Copy from pt address 0x%x for process %d\n",pt_info.pt[i].vPage,new);
                // The page is saved in the swap file, which will be associated with the new PID.
                storeSwapFrame(pt_info.pt[i].vPage,new,pt_info.firstfreepaddr+i*PAGE_SIZE); 
            }
            else{ //There is a valid page that can be used to store the page
                pt_info.pt[pos].ctl = 0;
                addInPT(pt_info.pt[i].vPage,new,pos);
                pt_info.pt[pos].ctl = SET_VALBITONE(pt_info.pt[pos].ctl);
                //It's a copy within RAM, memmove can be used. The reason to use PADDR_TO_KVADDR is explained in swapfile.c
                memmove((void *)PADDR_TO_KVADDR(pt_info.firstfreepaddr + pos*PAGE_SIZE),(void *)PADDR_TO_KVADDR(pt_info.firstfreepaddr + i*PAGE_SIZE), PAGE_SIZE); 
                KASSERT(GET_IOBIT(pt_info.pt[pos].ctl)==0);
                KASSERT(GET_TLBBIT(pt_info.pt[pos].ctl)==0);
                KASSERT(GET_SWAPBIT(pt_info.pt[pos].ctl)==0);
                KASSERT(GET_KBIT(pt_info.pt[pos].ctl)==0);
            }
        }
    }

    #if OPT_DEBUG
    printPageLists(new);
    #endif

}


void prepareCopyPT(pid_t pid){

    for(int i=0;i<pt_info.ptSize;i++){
        if(pt_info.pt[i].pid == pid && GET_KBIT(pt_info.pt[i].vPage) == 0 && GET_VALBIT(pt_info.pt[i].ctl) != 0){
            KASSERT(GET_IOBIT(pt_info.pt[i].ctl)==0);
            //To freeze the current situation we set the swap bit to 1.  This is done to
            // avoid inconsistencies between the situation at the beginning and at the end of the swapping process.
            pt_info.pt[i].ctl = SET_SWAPBITONE(pt_info.pt[i].ctl); 
        }
    }
}

void endCopyPT(pid_t pid){

    for(int i=0;i<pt_info.ptSize;i++){
        if(pt_info.pt[i].pid == pid && GET_KBIT(pt_info.pt[i].ctl) == 0 && GET_VALBIT(pt_info.pt[i].ctl) != 0){
            KASSERT(GET_SWAPBIT(pt_info.pt[i].ctl)!=0);
            pt_info.pt[i].ctl = SET_SWAPBITZERO(pt_info.pt[i].ctl); 
        }
    }

    /** As the pages that were previously swapped can now be selected as victims, 
     * all processes waiting on the condition variable are awakened.
    **/
    lock_acquire(pt_info.pt_lock);
    cv_broadcast(pt_info.pt_cv,pt_info.pt_lock); 
    lock_release(pt_info.pt_lock);

}
