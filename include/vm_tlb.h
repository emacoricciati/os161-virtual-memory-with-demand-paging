#ifndef _VM_TLB_H_
#define _VM_TLB_H_

#include "types.h"
#include "syscall.h"
#include "proc.h"
#include "opt-debug.h"

pid_t previous_pid;
pid_t old_pid;

/**
 * Select the victim in the TLB and returns its index.
*/
int tlbVictim(void);

/* Check if the address is in the text segment (code segment, it's not writable) */
int segmentIsReadOnly(vaddr_t vaddr);

/**
 * Insert a new entry in the TLB. It receives the fault address (virtual) and the 
 * corresponding physical one received by the page table. 
 * It finds a space to insert the entry (vaddr, paddr) using tlbVictim.
*/
int tlbInsert(vaddr_t vaddr, paddr_t faultpaddr);

/**
 * Check if the entry in the TLB at index i is valid or not.
*/
int tlbEntryIsValid(int i);

/**
 * Invalidate the TLB when there is a switch from a process to another. Indeed, 
 * the TLB is common for all processes and does not have a "pid" field.
*/
void tlbInvalidate(void);

/**
 * Used for debugging. Print the content of the TLB.
*/
void tlbPrint(void);


#endif