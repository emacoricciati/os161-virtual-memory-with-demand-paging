#ifndef _SWAPFILE_H_
#define _SWAPFILE_H_

#include "types.h"
#include "addrspace.h"
#include "kern/fcntl.h"
#include "uio.h"
#include "vnode.h"
#include "copyinout.h"
#include "lib.h"
#include "vfs.h"
#include "stats.h"
#include "synch.h"
#include "proc.h"
#include "vm.h"
#include "opt-debug.h"
#include "spl.h"
#include "current.h"

/**
 * Swapfile data structure
 */
struct swapFile{
    struct swapPage **textPages; // Array of lists containing text pages in the swap file (one list per PID)
    struct swapPage **dataPages; // Array of lists containing data pages in the swap file (one list per PID)
    struct swapPage **stackPages; // Array of lists containing stack pages in the swap file (one list per PID)
    struct swapPage *freePages; // List of available (free) pages in the swap file
    void *kbuf;//Buffer for copying of swap pages
    struct vnode *v; //vnode swapfile
    int sizeSF; //Number of pages stored in the swapfile
};

/**
 * Info on a single page of the swapfile
*/
struct swapPage{
    vaddr_t vaddr; //Virtual address of the stored page
    int isStoreOp; // Flag indicating whether a store operation is being performed on a specific page
    struct swapPage *next; // Pointer to the next page in the list
    paddr_t swapOffset; // Position of the swap element within the swap file
    struct cv *operationCV; // Used to wait for the completion of the store operation
    struct lock *operationLock; // Required to safely perform cv_wait

};

/**
 * This function restores a frame back into RAM.
 *
 * @param vaddr_t: virtual address that triggered the page fault
 * @param pid_t: process ID
 * @param paddr_t: physical address of the RAM frame to be used
 * 
 * @return 1 if the page was found in the swap file, 0 otherwise
*/
int loadSwapFrame(vaddr_t, pid_t, paddr_t);

/**
 * This function writes a frame into the swap file.
 * If the swap file size exceeds 9MB, it triggers a kernel panic.
 *
 * @param vaddr_t: virtual address that triggered the page fault
 * @param pid_t: process ID
 * @param paddr_t: physical address of the RAM frame to be saved
 * 
 * @return -1 on errors, 0 otherwise
*/
int storeSwapFrame(vaddr_t, pid_t, paddr_t);

/**
 * This function sets up the swap file. Specifically, it allocates the necessary data structures and opens the file that will hold the pages.
*/
int initSwapfile(void);


/**
 * When a process ends, we free all its pages stored in the swap file.
 * 
 * @param pid_t: process ID of the terminated process.
*/
void freeProcessPagesInSwap(pid_t);

/**
 * When a fork is executed, we duplicate all the pages of the old process for the new process as well.
 * 
 * @param pid_t: process ID of the original process.
 * @param pid_t: process ID of the new process.
*/
void duplicateSwapPages(pid_t, pid_t);

/**
 * (DEBUG) Given the process ID, it prints the text, data, and stack lists.
 * 
 * @param pid_t: process ID of the target process.
*/
void printPageLists(pid_t);

/**
 * After the entire program finishes, we reorder all the pages in the swap file.
 * Since lower offsets result in faster I/O, this function helps maintain performance.
*/
void optimizeSwapfile(void);

#endif /* _SWAPFILE_H_ */