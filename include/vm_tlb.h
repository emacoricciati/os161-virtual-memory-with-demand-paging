#ifndef _VM_TLB_H_
#define _VM_TLB_H_

#include "types.h"
#include "syscall.h"
#include "proc.h"
#include "opt-debug.h"

pid_t previous_pid;
pid_t old_pid;

/*
Returns the index of the victim selected (Round Robin) in the TLB.
*/ 
int tlbVictim(void);

/*
Defines if, given the virtual address of a frame, it is read only or not.
*/
int segmentIsReadOnly(vaddr_t vaddr);

/*
Write a new entry into the TLB.
- input parameters: the fault address (virtual) and physical address (given by the page table)
 */
int tlbInsert(vaddr_t vaddr, paddr_t faultpaddr);

/*
Check if the entry in the TLB at index i is valid or not.
*/
int tlbEntryIsValid(int i);

/*
Invalidate the TLB when there is a switch from a process to another (there is no PID field).
*/
void tlbInvalidate(void);

/*
Print the content of the TLB.
*/
void tlbPrint(void);


#endif