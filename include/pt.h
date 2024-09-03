#ifndef _PT_H_
#define _PT_H_

//aggiungere tlb

#include "types.h"
#include "addrspace.h"
#include "kern/errno.h"
#include "synch.h"

#define SET_VALBITZERO(val) (val & ~1)
#define SET_VALBITONE(val) (val | 1)
#define GET_VALBIT(val) (val & 1)
#define SET_REFBITONE(val) (val | 2)
#define SET_REFBITZERO(val) (val & ~2)
#define GET_REFBIT(val) (val & 2)
#define SET_KBITONE(val) (val | 4)                  //we defined an other bit to manage the kmalloc of a page
#define SET_KBITZERO(val) (val & ~4)
#define GET_KBIT(val) (val & 4)
#define SET_TLBBITZERO(val) (val & ~8)
#define SET_TLBBITONE(val) (val | 8)
#define GET_TLBBIT(val) (val & 8)
#define SET_IOBITONE(val) (val | 16)                
#define SET_IOBITZERO(val) (val & ~16)
#define GET_IOBIT(val) (val & 16)
#define SET_SWAPBITONE(val) (val | 32)                
#define SET_SWAPBITZERO(val) (val & ~32)
#define GET_SWAPBIT(val) (val & 32)




int pt_active;


struct pt_entry_s   //Page Table entry
{             
    pid_t pid;      // process id   
    vaddr_t vPage;  // virtual page
    uint8_t ctl;    // control bits:  Validity bit, Reference bit, Kalloc bit
} pt_entry;

struct pt_info_s
{
    struct pt_entry_s *pt;    // IPT
    int ptSize;             // IPT size: number of page entries
    paddr_t firstfreepaddr; // IPT starting address
    struct lock *pt_lock;   // Condition variable lock of IPT
    struct cv *pt_cv;       // Condition variable of IPT
    int *allocSize;         // Number of allocated pages to free
} pt_info;

/**
 * It initializes the page table.
 */
void initPT(void);

/**
 * This function gets the physical address from the IPT, then updated with the Hash table
 *
 * @param vaddr_t: virtual address
 * @param pid_t: pid of the process
 *
 *
 * @return -1 if the requested page isn't stored in the page table, physical address otherwise
 */
int getPAddressPT(vaddr_t, pid_t);

/**
 * This function is a wrapper for the following process:
 *  if physical frame found return directly the current physical address
 *  if not found find a position to insert the new element
 *  if all full find a victim
 *  return the physical position of the page
 *
 * @param vaddr_t: virtual address
 *
 *
 * @return physical address found inside the IPT
 */
paddr_t getFramePT(vaddr_t);

/**
 * This function adds a new entry in the IPT
 *  return the physical address of the page
 *
 * @param pid_t: pid
 * @param vaddr_t: virtual address
 * @param int: index
 * 
 * @return physical address of the page
 */
paddr_t addInPT(vaddr_t, pid_t, int);

/**
 * This function finds a victim in the IPT and the updates the hash table.
 *  it uses a second chance algorithm based on TLB presence and reference bit
 * @param vaddr_t: virtual address
 * @param pid_t: pid of the process
 *
 *
 * @return physical address found inside the IPT
 */
int findVictim(vaddr_t, pid_t);

/**
 * This function frees all the pages inside the IPT
 *
 * @param vaddr_t: virtual address
 * @param pid_t: pid of the process
 *
 * @return void
 */
void freePages(pid_t);

/**
 * This function inserts in the IPT some kernel memory in a contiguous way
 *
 * @param int: the number of pages to allocate
 *
 * @return physical address found inside the IPT
 */
paddr_t getContiguousPages(int);

void freePContiguousPages(paddr_t addr);

/**
 * This function frees the contiguous pages allocated in IPT
 *
 * @param vaddr_t: virtual address
 *
 *
 */
void freeContiguousPages(vaddr_t);
/**
 * This function advices that a page is removed from TLB.
 *
 * @param vaddr_t: virtual address
 *
 *
 * @return 1 if everything ok, -1 otherwise
 */
int tlbUpdateBit(vaddr_t, pid_t);
void removeFromPT(vaddr_t, pid_t);
int getIndexFromPT(vaddr_t, pid_t);
void prepareCopyPT(pid_t pid);
#endif