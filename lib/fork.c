// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

// Assembly language pgfault entrypoint defined in lib/pfentry.S.
// We need this because we need to re-register the user-level page fault handler for child
extern void _pgfault_upcall(void);
// A wrapper, to get the page table entry at virtual address addr
// In the case of page table does not exist, return 0(representing no mapping)
static pte_t get_pte(void* addr)
{
	if((uvpd[PDX((uintptr_t)addr)] & PTE_P) == 0)
		return 0;
	return uvpt[PGNUM((uintptr_t)addr)];
}


//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int error_code;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	// First, get the page table entry at this addr
	pte_t pte = get_pte(addr);
	// check the permission bits of error code and this page table entry
	if(!(err & FEC_WR) || !(pte & PTE_COW))
	{
		cprintf("[%08x] user fault va %08x ip %08x\n", sys_getenvid(), addr, utf->utf_eip);
		panic("Page Fault!");
	}

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.
	// Question: Why Do we need three syscalls here?
	// LAB 4: Your code here.
	// Round down addr
	uintptr_t start_addr = ROUNDDOWN((uintptr_t)addr, PGSIZE);
	if((error_code = sys_page_alloc(0, PFTEMP, PTE_W | PTE_U | PTE_P)) < 0)
		panic("Page Alloc Failed: %e", error_code);
	memmove((void*)PFTEMP, (void*)start_addr, PGSIZE);
	if((error_code = sys_page_map(0, (void*)PFTEMP, 0, (void*)start_addr, PTE_W | PTE_U | PTE_P)) < 0)
		panic("Page Map Failed: %e", error_code);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	void* addr = (void*)(pn << PTXSHIFT);
	int error_code = 0;
	// LAB 4: Your code here.
	// First check whether this virtual page exists
	// The syscall also does check, but we need this double check because
	// Then we need to use this PTE to check whether it is read-only
	pte_t pte = get_pte((void*)(pn * PGSIZE));
	if(!(pte & PTE_P))
		return -1;
	// Case 1: This page is read-only, check uvpt for it
	if(!(pte & PTE_W) && !(pte & PTE_COW))
	{
		if((error_code = sys_page_map(0, addr, envid, addr, PTE_U | PTE_P)) < 0)
			panic("Page Map Failed: %e", error_code);
	}
	// LAB5: page share copes with pages concerning fds 
	else if(pte & PTE_SHARE)
	{
		if((error_code = sys_page_map(0, addr, envid, addr, pte & PTE_SYSCALL)) < 0)
			panic("Page Map Failed: %e", error_code);
	}
	else
	{
		// Map this page to the child, and make it COW
		if((error_code = sys_page_map(0, addr, envid, addr, PTE_U | PTE_COW | PTE_P)) < 0)
			panic("Page Map Failed: %e", error_code);
		// Make this page mapping in curenv COW
		if((error_code = sys_page_map(0, addr, 0, addr, PTE_U | PTE_COW | PTE_P)) < 0)
			panic("Page Map Failed: %e", error_code);
	}
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	int error_code = 0;
	// LAB 4: Your code here.
	set_pgfault_handler(pgfault);
	// use exofork syscall 
	envid_t curenv_id = sys_exofork();
	// Question: Why do we not register user-level page fault handler here?
	// So we just need to write once
	// Answer: Child env can be run only after parent has changed its status to ENV_RUNNABLE
	// but if child starts running, it must be running on a stack, then a page fault happens
	// But we have not registered a user-level page fault handler yet!
	// So parent have to take the responsibility to register child's user-level page fault handler
	if(curenv_id == 0)
	{
		// For child env
		// Fix thisenv
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	else
	{
		// For parent envs, do the real stuff
		// First, set up child's page fault handler
		if((error_code = sys_page_alloc(curenv_id, (void*)(UXSTACKTOP - PGSIZE), PTE_W | PTE_U | PTE_P)) < 0)
			panic("Page Alloc Failed: %e", error_code);
		// Question: Why should we make child page COW first, then parent's page?
		// Because If parent modifies the page, then child will get the wrong stuff
		// Second, register the user-level page fault handler. Here, we have to make system call
		// by ourself, because function in pgfault.c has been called
		sys_env_set_pgfault_upcall(curenv_id, _pgfault_upcall);
		// Then, copy address space 
		for(unsigned int i = 0; i < PGNUM(UTOP); i++)
		{
			// Remember to ignore exception stack
			if(i == PGNUM(UXSTACKTOP - PGSIZE))
				continue;
			// check whether this page table entry is valid(whether there exists a mapping)
			if(!(get_pte((void*)(i * PGSIZE)) & PTE_P))
				continue;
			// call duppage now
			if(duppage(curenv_id, i) < 0)
				panic("Duppage Failed!");
		}
		// Make child runnable
		sys_env_set_status(curenv_id, ENV_RUNNABLE);
		// Time to return now
		return curenv_id;
	}
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
