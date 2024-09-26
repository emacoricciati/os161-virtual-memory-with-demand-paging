#ifndef _VMSTATS_H_
#define _VMSTATS_H_

#include <types.h>
#include <lib.h>
#include <spinlock.h>

// Stats types
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

// Structure for TLB statistics
struct statistics_tlb {
    uint32_t tlb_faults;
    uint32_t tlb_faults_with_free;
    uint32_t tlb_faults_with_replace;
    uint32_t tlb_invalidations;
    uint32_t tlb_reloads;
    struct spinlock lock; 
};

// Structure for PT statistics
struct statistics_pt {
    uint32_t pt_faults_zeroed;
    uint32_t pt_faults_disk;
    uint32_t pt_faults_from_elf;
    uint32_t pt_faults_from_swapfile;
    uint32_t pt_swapfile_writes;
    struct spinlock lock; 
};

// Global variables
extern struct statistics_tlb statistics_tlb;
extern struct statistics_pt statistics_pt;

// Function prototypes
void initializeStatistics(void);
void incrementStatistics(int type);
uint32_t returnTLBStatistics(int type);
uint32_t returnPTStatistics(int type);
uint32_t returnSWStatistics(int type);
void constraintsCheck(uint32_t faults, uint32_t free, uint32_t replace, uint32_t reload, uint32_t disk, uint32_t zeroed, uint32_t elf, uint32_t swapfile);
void printStatistics(void);

#endif

