#include "stats.h"

// Global variables
struct statistics_tlb statistics_tlb = {0};
struct statistics_pt statistics_pt = {0};

// Init statistics
void initializeStatistics(void) {
    spinlock_init(&statistics_tlb.lock);
    spinlock_init(&statistics_pt.lock);
    
    statistics_tlb.tlb_faults = 0;
    statistics_tlb.tlb_faults_with_free = 0;
    statistics_tlb.tlb_faults_with_replace = 0;
    statistics_tlb.tlb_invalidations = 0;
    statistics_tlb.tlb_reloads = 0;
    
    statistics_pt.pt_faults_zeroed = 0;
    statistics_pt.pt_faults_disk = 0;
    statistics_pt.pt_faults_from_elf = 0;
    statistics_pt.pt_faults_from_swapfile = 0;
    statistics_pt.pt_swapfile_writes = 0;
}

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

uint32_t returnPTStatistics(int type) {
    uint32_t result;

    spinlock_acquire(&statistics_pt.lock);

    switch (type) {
        case FAULT_ZEROED:
            result = statistics_pt.pt_faults_zeroed;
            break;
        case FAULT_DISK:
            result = statistics_pt.pt_faults_disk;
            break;
        case FAULT_FROM_ELF:
            result = statistics_pt.pt_faults_from_elf;
            break;
        case FAULT_FROM_SWAPFILE:
            result = statistics_pt.pt_faults_from_swapfile;
            break;
        default:
            result = 0;
            break;
    }

    spinlock_release(&statistics_pt.lock);
    return result;
}

uint32_t returnSWStatistics(int type) {
    uint32_t result;

    spinlock_acquire(&statistics_pt.lock);

    switch (type) {
        case SWAPFILE_WRITES:
            result = statistics_pt.pt_swapfile_writes;
            break;
        default:
            result = 0;
            break;
    }

    spinlock_release(&statistics_pt.lock);
    return result;
}

void constraintsCheck(uint32_t faults, uint32_t free, uint32_t replace, uint32_t reload, uint32_t disk, uint32_t zeroed, uint32_t elf, uint32_t swapfile) {
    if (faults == (free + replace)) {
        kprintf("CORRECT: the sum of tlb_faults_with_free and tlb_faults_with_replace is equal to tlb_faults\n");
    } else {
        kprintf("WARNING: the sum of tlb_faults_with_free and tlb_faults_with_replace is not equal to tlb_faults\n");
    }

    if (faults == (reload + disk + zeroed)) {
        kprintf("CORRECT: the sum of tlb_reload, pt_faults_disk and pt_faults_zeroed is equal to tlb_faults\n");
    } else {
        kprintf("WARNING: the sum of tlb_reload, pt_faults_disk and pt_faults_zeroed is not equal to tlb_faults\n");      
    }

    if (disk == (elf + swapfile)) {
        kprintf("CORRECT: the sum of pt_faults_from_elf and pt_faults_from_swapfile is equal to tlb_faults_disk\n\n");
    } else {
        kprintf("WARNING: the sum of pt_faults_from_elf and pt_faults_from_swapfile is not equal to tlb_faults_disk\n\n");      
    }
}

void printStatistics(void) {
    uint32_t tlb_faults = returnTLBStatistics(FAULT);
    uint32_t tlb_faults_with_free = returnTLBStatistics(FAULT_WITH_FREE);
    uint32_t tlb_faults_with_replace = returnTLBStatistics(FAULT_WITH_REPLACE);
    uint32_t tlb_invalidations = returnTLBStatistics(INVALIDATION);
    uint32_t tlb_reloads = returnTLBStatistics(RELOAD);
    uint32_t pt_faults_zeroed = returnPTStatistics(FAULT_ZEROED);
    uint32_t pt_faults_disk = returnPTStatistics(FAULT_DISK);
    uint32_t pt_faults_from_elf = returnPTStatistics(FAULT_FROM_ELF);
    uint32_t pt_faults_from_swapfile = returnPTStatistics(FAULT_FROM_SWAPFILE);
    uint32_t pt_swapfile_writes = returnSWStatistics(SWAPFILE_WRITES);

    kprintf("\nTLB statistics:\n"
            "\tTLB faults = %d\n"
            "\tTLB Faults with Free = %d\n"
            "\tTLB Faults with Replace = %d\n"
            "\tTLB Invalidations = %d\n"
            "\tTLB Reloads = %d\n",
            tlb_faults, tlb_faults_with_free, tlb_faults_with_replace, tlb_invalidations, tlb_reloads);

    kprintf("PT statistics:\n"
            "\tPage Faults (Zeroed) = %d\n"
            "\tPage Faults (Disk) = %d\n"
            "\tPage Faults from ELF = %d\n"
            "\tPage Faults from Swapfile = %d\n",
            pt_faults_zeroed, pt_faults_disk, pt_faults_from_elf, pt_faults_from_swapfile);

    kprintf("\nSwapfile writes = %d\n\n", pt_swapfile_writes);

    constraintsCheck(tlb_faults, tlb_faults_with_free, tlb_faults_with_replace, tlb_reloads, pt_faults_zeroed, pt_faults_disk, pt_faults_from_elf, pt_faults_from_swapfile);
}
