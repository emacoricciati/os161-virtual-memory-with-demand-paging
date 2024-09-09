#include "segments.h"

/**
 * This function loads a page from the ELF file into the specified virtual address.
 * 
 * @param v: the vnode representing the ELF file
 * @param offset: the offset within the ELF file
 * @param vaddr: the virtual address where the data will be stored
 * @param memsize: the amount of memory to read
 * @param filesize: the amount of data to read from the file
 * 
 * @return 0 if the operation is successful, otherwise returns the error code from VOP_READ
 * 
*/
#if OPT_FINAL

static int loadELFPage(struct vnode *v, off_t offset, vaddr_t vaddr, size_t memsize, size_t filesize)
{

	struct iovec iov;
	struct uio u;
	int result;

	if (filesize > memsize) {
		kprintf("ELF: Warning - segment file size is greater than segment memory size\n");
		filesize = memsize;
	}

	DEBUG(DB_VM,"ELF: Loading %lu bytes to address 0x%lx\n",(unsigned long) filesize, (unsigned long) vaddr);

    /**
     * It's not possible to use uio_kinit because it does not allow setting different values for 
     * iov_len and uio_resid, which is crucial in our case.
     * Refer to testbin/zero for further details.
     */

	iov.iov_ubase = (userptr_t)vaddr;
	iov.iov_len = memsize;		 // memory space length
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = filesize;  // number of bytes to read from the file
	u.uio_offset = offset;
	u.uio_segflg = UIO_SYSSPACE;
	u.uio_rw = UIO_READ;
	u.uio_space = NULL;

	result = VOP_READ(v, &u);// reading operation

	return result;
}

int loadPage(vaddr_t vaddr, pid_t pid, paddr_t paddr){

    int found, result;
    struct addrspace *as;
    int sz=PAGE_SIZE, memsz=PAGE_SIZE;
	size_t additional_offset=0;

    /** Check if the page has already been loaded from the ELF file, 
     * meaning it is currently stored in the swap file
    **/

    found = loadSwapFrame(vaddr, pid, paddr); 

    if(found){
        return 0;
    }

	as = proc_getas();

	#if OPT_DEBUG
	printPageLists(pid);
	#endif

	DEBUG(DB_VM,"Process %d is attempting to read the ELF file\n",pid);

    /** At this point, it means the page was not found in the swap file, 
     * so we need to load it from the ELF file.
    **/


	// Check if the virtual address belongs to the text segment

    if(vaddr>=as->as_vbase1 && vaddr <= as->as_vbase1 + as->as_npages1 * PAGE_SIZE ){

        DEBUG(DB_VM,"Loading code: ");
        
		incrementStatistics(FAULT_DISK); //update statistics

        /**
         * Certain programs may start their text/data segments at addresses that are not aligned to a page (refer to testbin/bigfork for an example). This information gets lost during 
         * as_create, as as->as_vbase reflects an address that is aligned to a page. To resolve this issue, an extra field called offset should be incorporated into the as struct. 
         * This offset is utilized when accessing the first page, provided it is non-zero. In such cases, the page is filled with zeros, and the loading of the ELF file commences with 
         * additional_offset used as the offset within the frame.
         */

		if(as->initial_offset_text!=0 && (vaddr - as->as_vbase1)==0){
			bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
			additional_offset=as->initial_offset_text; // othwerwise it's 0
			if(as->prog_head_text.p_filesz>=PAGE_SIZE-additional_offset){
                // filesz is large enough to occupy the remaining portion of the block, so PAGE_SIZE - additional_offset bytes are loaded.
				sz=PAGE_SIZE-additional_offset;
			}
			else{
                // filesz is insufficient to fill the remaining portion of the block, so only filesz bytes are loaded.
				sz=as->prog_head_text.p_filesz;
			}
		}
		else{

            // If the remaining segment does not fully occupy the entire page, it is necessary to zero-fill the page before loading the data.
			if(as->prog_head_text.p_filesz+as->initial_offset_text - (vaddr - as->as_vbase1)<PAGE_SIZE){ 
                // To prevent extra TLB faults, we treat the provided physical address as if it belongs to the kernel. This allows the address translation to simply be vaddr - 0x80000000.
				bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
                // The file size of the last page is computed, taking into account as->initial_offset as well.
				sz=as->prog_head_text.p_filesz+as->initial_offset_text - (vaddr - as->as_vbase1);
                // The memory size of the last page is calculated, taking into consideration as->initial_offset as well. 
				memsz=as->prog_head_text.p_memsz+as->initial_offset_text - (vaddr - as->as_vbase1); 
			}

            /**  This check is essential to prevent problems with programs where filesz < memsz. 
             * Without this check, the page would not be zero-filled, leading to errors. 
             * For further insight, debugging testbin/zero can help analyze 
             * the difference between memsz and filesz.
            **/

			if((int)(as->prog_head_text.p_filesz+as->initial_offset_text) - (int)(vaddr - as->as_vbase1)<0){
				bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
                DEBUG(DB_VM, "Loading ELF at physical address 0x%x (virtual address: 0x%x). Program with filesz < memsz\n", paddr, vaddr);
                // The function returns directly to avoid executing a read of 0 bytes.
				return 0;
			}
		}

		DEBUG(DB_VM, "Loading ELF at physical address 0x%x (virtual address: 0x%x)\n", paddr, vaddr);

        result = loadELFPage(as->v, as->prog_head_text.p_offset+(vaddr - as->as_vbase1), PADDR_TO_KVADDR(paddr+additional_offset), memsz, sz-additional_offset);//We load the page
		if (result) {
            panic("Fatal error: read from text segment failed");
		}

		incrementStatistics(FAULT_FROM_ELF); //update statistics

        return 0;
    }

    /**
	 * Check if the virtual address provided belongs to the data segment. (Same proc as above)
	*/
    if(vaddr>=as->as_vbase2 && vaddr <= as->as_vbase2 + as->as_npages2 * PAGE_SIZE ){

		DEBUG(DB_VM, "Loading data: virtual address = 0x%x, physical address = 0x%x\n", vaddr, paddr);

		incrementStatistics(FAULT_DISK);

		if(as->initial_offset_data!=0 && (vaddr - as->as_vbase2)==0){
			bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
			additional_offset=as->initial_offset_data;
			if(as->prog_head_data.p_filesz>=PAGE_SIZE-additional_offset){
				sz=PAGE_SIZE-additional_offset;
			}
			else{
				sz=as->prog_head_data.p_filesz;
			}
		}
		else{
			if(as->prog_head_data.p_filesz+as->initial_offset_data - (vaddr - as->as_vbase2)<PAGE_SIZE){ 
				bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
				sz=as->prog_head_data.p_filesz+as->initial_offset_data - (vaddr - as->as_vbase2);
				memsz=as->prog_head_data.p_memsz+as->initial_offset_data - (vaddr - as->as_vbase2);
			}

			if((int)(as->prog_head_data.p_filesz+as->initial_offset_data) - (int)(vaddr - as->as_vbase2)<0){
				bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
				incrementStatistics(FAULT_FROM_ELF);
                DEBUG(DB_VM, "Loading ELF at physical address 0x%x (virtual address: 0x%x)\n", paddr, vaddr);
				return 0;
			}
		}

        result = loadELFPage(as->v, as->prog_head_data.p_offset+(vaddr - as->as_vbase2),	PADDR_TO_KVADDR(paddr+additional_offset),memsz, sz);
		if (result) {
            panic("Error reading the data segment");
		}

		incrementStatistics(FAULT_FROM_ELF);

		DEBUG(DB_VM, "Loading ELF at physical address 0x%x (virtual address: 0x%x)\n", paddr, vaddr);

        return 0;
    }

    /**
     * Check whether the virtual address is part of the text segment.
     * This check is performed in this way because the stack grows from 0x80000000 (exclusive) to as->as_vbase2 + as->as_npages2 * PAGE_SIZE.
     */

    if(vaddr>as->as_vbase2 + as->as_npages2 * PAGE_SIZE && vaddr<USERSTACK){

		DEBUG(DB_VM,"Loading stack: ");

        DEBUG(DB_VM, "ELF: Loading 4096 bytes to address 0x%lx\n", (unsigned long) vaddr);

        // 0-fill the page, it's not needed to perform any kind of load.
        bzero((void*)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
		
		incrementStatistics(FAULT_ZEROED); //update statistics

        DEBUG(DB_VM, "Loading ELF at physical address 0x%x (virtual address: 0x%x)\n", paddr, vaddr);

        return 0;
    }

    /**
     * Error access outside the address space
     * Terminate the program due to illegal access
     */
    kprintf("Segmentation fault: process %d attempted to access 0x%x\n", pid, vaddr);

    sys__exit(-1);

    return -1;
}

#endif