#include "pt.h"
#include "spinlock.h"
#include "vm.h"
#include "mainbus.h"
#include "lib.h"
#include "current.h"
#include "proc.h"

int next_victim = 0; // second chance


static int findFreeEntryPT(){
    for(int i=0; i<pt_info.ptSize; i++){
        if(GET_VALBIT(pt_info.pt[i].ctl)==0 &&  GET_KBIT(pt_info.pt[i].ctl) == 0){
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
    nFrames = (mainbus_ramsize() - ram_stealmem(0))/PAGE_SIZE;               //number of frames in physical memory (= IPT size)
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

    pt_info.firstfreepaddr = ram_stealmem(0); //ram_stealmem(0) returns the first free physical address (=from where our IPT starts)
    pt_info.ptSize = nFrames/* - 1*/;  
    
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
    KASSERT(GET_KBIT(pt_info.pt[i].ctl)==0);

    return i * PAGE_SIZE + pt_info.firstfreepaddr; // send the paddr found
}

paddr_t getFramePT(vaddr_t v_addr){

    pid_t current_pid = curproc->p_pid;
    int val = getPAddressPT(v_addr,current_pid);
    paddr_t p_addr;
    if(val != -1){
        // virtual address is available in the pt
        p_addr = (paddr_t) (val);
        return p_addr;
    }

    // virtual address is not available in the page table, searching free entry in the pt
    int entry = findFreeEntryPT();
    if(entry != -1){
        // free entry available
        p_addr = addInPT(v_addr, current_pid, entry);
        return p_addr;
    }
    // free entry not available in the pt, find a victim
    entry = findVictim(v_addr, current_pid);
    return (paddr_t) ((entry * PAGE_SIZE) + pt_info.firstfreepaddr);
}

int findVictim(vaddr_t v_addr, pid_t pid){

    // circular buffer implementation
    int i, end = next_victim, n = 0; 

    for(i = next_victim;; i = (i+1)%pt_info.ptSize){
        // if the page can be replaced
        if(GET_KBIT(pt_info.pt[i].ctl) == 0){ 
            // second change algorithm
            if(GET_REFBIT(pt_info.pt[i].ctl) == 0){ // no check on validity, if validity bit = 0 it means that there is a free entry
                // victim is found
                KASSERT(GET_KBIT(pt_info.pt[i].ctl)==0); // no kmalloc
                KASSERT(GET_VALBIT(pt_info.pt[i].ctl)); // valid entry
                // Overwrite the entry
                pt_info.pt[i].pid = pid;
                pt_info.pt[i].vPage = v_addr;
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

void freePages(pid_t pid){  // frees all pages from PT and the list using pid

    for (int i = 0; i < pt_info.ptSize; i++){
        if (pt_info.pt[i].pid == pid){
            if(GET_VALBIT(pt_info.pt[i].ctl) && GET_KBIT(pt_info.pt[i].ctl)==0){ //We don't free: kmalloc pages                                   
                KASSERT(GET_VALBIT(pt_info.pt[i].ctl));
                KASSERT(GET_KBIT(pt_info.pt[i].ctl)==0);
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

    paddr_t test = KVADDR_TO_PADDR(addr);
    kprintf("%d",test);
    index = (KVADDR_TO_PADDR(addr) - pt_info.firstfreepaddr) / PAGE_SIZE;  //get the index in the IPT 
    n_contig_pages = pt_info.allocSize[index];        //get the number of contiguous allocated pages

   
    for(i=index;i<index+n_contig_pages;i++){
        KASSERT(GET_KBIT(pt_info.pt[i].ctl));                       //page assigned with kmalloc
        pt_info.pt[i].ctl = SET_VALBITZERO(pt_info.pt[i].ctl);      //valid bit = 0 -> page not valid anymore
        pt_info.pt[i].ctl = SET_KBITZERO(pt_info.pt[i].ctl);        //clear kmalloc bit                                               
    }

    pt_info.allocSize[index]=-1;                              //clear the number of contiguous allocated pages

    /*
    if(curthread->t_in_interrupt == false){                     //if I'm in the interrupt i cannot acquire a lock
        lock_acquire(pt_info.pt_lock);
        cv_broadcast(pt_info.pt_cv,pt_info.pt_lock);            //Since we freed some pages, we wake up the processes waiting on the cv.
        lock_release(pt_info.pt_lock);
    }
    else{
        cv_broadcast(pt_info.pt_cv,pt_info.pt_lock);
    }
    */

}

void freePContiguousPages(paddr_t addr){
    int i, index, n_contig_pages;

    index = (addr - pt_info.firstfreepaddr) / PAGE_SIZE;  //get the index in the IPT 
    n_contig_pages = pt_info.allocSize[index];        //get the number of contiguous allocated pages

   
    for(i=index;i<index+n_contig_pages;i++){
        KASSERT(GET_KBIT(pt_info.pt[i].ctl));                       //page assigned with kmalloc
        pt_info.pt[i].ctl = SET_VALBITZERO(pt_info.pt[i].ctl);      //valid bit = 0 -> page not valid anymore
        pt_info.pt[i].ctl = SET_KBITZERO(pt_info.pt[i].ctl);        //clear kmalloc bit                                               
    }

    pt_info.allocSize[index]=-1;                              //clear the number of contiguous allocated pages

    /*
    if(curthread->t_in_interrupt == false){                     //if I'm in the interrupt i cannot acquire a lock
        lock_acquire(pt_info.pt_lock);
        cv_broadcast(pt_info.pt_cv,pt_info.pt_lock);            //Since we freed some pages, we wake up the processes waiting on the cv.
        lock_release(pt_info.pt_lock);
    }
    else{
        cv_broadcast(pt_info.pt_cv,pt_info.pt_lock);
    }
    */

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

void removeFromPT(vaddr_t vad, pid_t pid) { // insert again in unusedptrlist ???
    int i=getIndexFromPT(vad, pid);
    if(i==-1){
        kprintf("page not found");
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
    pt_info.pt[index].ctl=0;
    pt_info.pt[index].ctl=SET_VALBITONE(pt_info.pt[index].ctl);
    return (paddr_t) (pt_info.firstfreepaddr + index*PAGE_SIZE);
}


paddr_t getContiguousPages(int nPages){
    int i, j, first=-1, prev=0;
    int firstIteration=0;

    if (nPages > pt_info.ptSize){
        panic("Can't do kmalloc, not enough memory"); //Impossible allocation
    }

    //Option 1: searching for contiguos npages in order to avoid swapping
    for (i = 0; i < pt_info.ptSize; i++){
        if(i!=0){           //Checking the validity of the previous entry
            prev = GET_KBIT(pt_info.pt[i-1].ctl) || (GET_VALBIT(pt_info.pt[i-1].ctl) && GET_REFBIT(pt_info.pt[i-1].ctl)) ? 1 : 0; 
        }
        if(GET_VALBIT(pt_info.pt[i].ctl)==0 && GET_KBIT(pt_info.pt[i].ctl)==0 && (i==0 || prev)){  //checking if the current entry is not valid while the previous one was valid (or if the first entry is not valid)
            first=i; 
        } 
        if(first>=0 && GET_VALBIT(pt_info.pt[i].ctl)==0 && GET_KBIT(pt_info.pt[i].ctl)==0 && i-first==nPages-1){ //We found npages contiguous entries
            //we are allocating with kmalloc
            for(j=first;j<=i;j++){
                KASSERT(GET_KBIT(pt_info.pt[j].vPage)==0);          
                KASSERT(GET_VALBIT(pt_info.pt[j].ctl)==0);
                pt_info.pt[j].ctl=SET_VALBITONE(pt_info.pt[j].ctl);   //Set pages as valid
                pt_info.pt[j].ctl=SET_KBITONE(pt_info.pt[j].ctl);     //This page can't be swapped out until when we perform a free on it
                //pt_info.pt[j].pid = curproc->p_pid;       //vaddr and pid are useless here since kernel uses a different address translation (i.e. it doesn't access the IPT to get their physical address)
            }
            pt_info.allocSize[first] = nPages; //We save in position first the number of contiguous pages allocated. It'll be useful while freeing
            return first * PAGE_SIZE + pt_info.firstfreepaddr;
        }
    }


    //Option 2: we perform victim selection because table is full
    while(1){  //Exit only when we find n contiguous victims
        for (i = next_victim; i < pt_info.ptSize; i++){
            if (GET_KBIT(pt_info.pt[i].vPage) == 0){ //Checking if the entry can be removed
                if(GET_REFBIT(pt_info.pt[i].ctl) && GET_VALBIT(pt_info.pt[i].ctl)){ //If the page is valid and has reference=1 we set reference=0 (due to second chance algorithm) and we continue
                    pt_info.pt[i].ctl = SET_REFBITZERO(pt_info.pt[i].ctl);
                    continue;
                }
                int valid= GET_KBIT(pt_info.pt[i].ctl) || (GET_VALBIT(pt_info.pt[i].ctl) && GET_REFBIT(pt_info.pt[i].ctl)) ? 1 : 0;
                if ((GET_REFBIT(pt_info.pt[i].ctl) == 0 || GET_VALBIT(pt_info.pt[i].ctl) == 0) && (i==0 || valid)){//If the current entry can be removed and the previous is valid, i is the start of the interval
                    first = i;
                }
                if(first>=0 && (GET_REFBIT(pt_info.pt[i].ctl) == 0 || GET_VALBIT(pt_info.pt[i].ctl) == 0) && i-first==nPages-1){ //We found npages contiguous entries that can be removed
                    for(j=first;j<=i;j++){
                        KASSERT(GET_KBIT(pt_info.pt[j].vPage)==0);
                        KASSERT(GET_REFBIT(pt_info.pt[j].ctl)==0 || GET_VALBIT(pt_info.pt[j].ctl)==0);
                        pt_info.pt[j].pid = curproc->p_pid;
                        pt_info.pt[j].vPage = SET_KBITONE(pt_info.pt[j].ctl); //To remember that this page can't be swapped out until when we perform a free
                        pt_info.pt[j].ctl = SET_VALBITONE(pt_info.pt[j].ctl); //Set pages as valid
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