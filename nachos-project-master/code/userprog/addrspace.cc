// addrspace.cc
//	Routines to manage address spaces (executing user programs).
//
//	In order to run a user program, you must:
//
//	1. link with the -n -T 0 option
//	2. run coff2noff to convert the object file to Nachos format
//		(Nachos object code format is essentially just a simpler
//		version of the UNIX executable object code format)
//	3. load the NOFF file into the Nachos file system
//		(if you are using the "stub" file system, you
//		don't need to do this last step)
//
// Copyright (c) 1992-1996 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "main.h"
#include "addrspace.h"
#include "machine.h"
#include "synch.h"

struct FrameRecord {
    AddrSpace *owner;
    unsigned int vpn;
    bool occupied;
};

static FrameRecord gFrameTable[NumPhysPages];
static unsigned int gClockHand = 0;
static unsigned int gTlbHand = 0;

int AllocateFrame(AddrSpace *space, unsigned int vpn);
int SelectVictimFrame();

//----------------------------------------------------------------------
// SwapHeader
// 	Do little endian to big endian conversion on the bytes in the
//	object file header, in case the file was generated on a little
//	endian machine, and we're now running on a big endian machine.
//----------------------------------------------------------------------

static void SwapHeader(NoffHeader *noffH) {
    noffH->noffMagic = WordToHost(noffH->noffMagic);
    noffH->code.size = WordToHost(noffH->code.size);
    noffH->code.virtualAddr = WordToHost(noffH->code.virtualAddr);
    noffH->code.inFileAddr = WordToHost(noffH->code.inFileAddr);
#ifdef RDATA
    noffH->readonlyData.size = WordToHost(noffH->readonlyData.size);
    noffH->readonlyData.virtualAddr =
        WordToHost(noffH->readonlyData.virtualAddr);
    noffH->readonlyData.inFileAddr = WordToHost(noffH->readonlyData.inFileAddr);
#endif
    noffH->initData.size = WordToHost(noffH->initData.size);
    noffH->initData.virtualAddr = WordToHost(noffH->initData.virtualAddr);
    noffH->initData.inFileAddr = WordToHost(noffH->initData.inFileAddr);
    noffH->uninitData.size = WordToHost(noffH->uninitData.size);
    noffH->uninitData.virtualAddr = WordToHost(noffH->uninitData.virtualAddr);
    noffH->uninitData.inFileAddr = WordToHost(noffH->uninitData.inFileAddr);

#ifdef RDATA
    DEBUG(dbgAddr, "code = " << noffH->code.size
                             << " readonly = " << noffH->readonlyData.size
                             << " init = " << noffH->initData.size
                             << " uninit = " << noffH->uninitData.size << "\n");
#endif
}

//----------------------------------------------------------------------
// AddrSpace::AddrSpace
// 	Create an address space to run a user program.
//	Set up the translation from program memory to physical
//	memory.  For now, this is really simple (1:1), since we are
//	only uniprogramming, and we have a single unsegmented page table
//----------------------------------------------------------------------

AddrSpace::AddrSpace() {
    pageTable = NULL;
    numPages = 0;
    executable = NULL;
    swapFile = NULL;
    swapFileName = NULL;
    pageInfo = NULL;
    bzero(&noffHeader, sizeof(noffHeader));
}

//----------------------------------------------------------------------
// AddrSpace::~AddrSpace
// 	Dealloate an address space.
//----------------------------------------------------------------------

AddrSpace::~AddrSpace() {
    int i;
    for (i = 0; i < numPages; i++) {
        if (pageTable[i].valid && pageTable[i].physicalPage >= 0) {
            InvalidateTlbForPhysicalPage(pageTable[i].physicalPage);
            kernel->gPhysPageBitMap->Clear(pageTable[i].physicalPage);
            gFrameTable[pageTable[i].physicalPage].occupied = false;
            gFrameTable[pageTable[i].physicalPage].owner = NULL;
        }
    }
    delete[] pageTable;
    delete[] pageInfo;
    delete executable;
    delete swapFile;
    if (swapFileName != NULL) {
        kernel->fileSystem->Remove(swapFileName);
        delete[] swapFileName;
    }
}

//----------------------------------------------------------------------
// AddrSpace::Load
// 	Load a user program into memory from a file.
//
//	Assumes that the page table has been initialized, and that
//	the object code file is in NOFF format.
//
//	"fileName" is the file containing the object code to load into memory
//----------------------------------------------------------------------

AddrSpace::AddrSpace(char *fileName) {
    pageTable = NULL;
    numPages = 0;
    executable = kernel->fileSystem->Open(fileName);
    swapFile = NULL;
    swapFileName = NULL;
    pageInfo = NULL;

    unsigned int i, size;

    if (executable == NULL) {
        DEBUG(dbgFile, "\n Error opening file.");
        return;
    }

    executable->ReadAt((char *)&noffHeader, sizeof(noffHeader), 0);
    if ((noffHeader.noffMagic != NOFFMAGIC) &&
        (WordToHost(noffHeader.noffMagic) == NOFFMAGIC))
        SwapHeader(&noffHeader);
    ASSERT(noffHeader.noffMagic == NOFFMAGIC);

    size = noffHeader.code.size + noffHeader.initData.size +
           noffHeader.uninitData.size +
           UserStackSize;  // we need to increase the size
                           // to leave room for the stack
    numPages = divRoundUp(size, PageSize);
    size = numPages * PageSize;

    DEBUG(dbgAddr, "Initializing address space: " << numPages << ", " << size);

    pageTable = new TranslationEntry[numPages];
    pageInfo = new PageInfo[numPages];
    for (i = 0; i < numPages; i++) {
        pageTable[i].virtualPage = i;
        pageTable[i].physicalPage = -1;
        pageTable[i].valid = FALSE;
        pageTable[i].use = FALSE;
        pageTable[i].dirty = FALSE;
        pageTable[i].readOnly = FALSE;
        pageInfo[i].hasSwapCopy = false;
        pageInfo[i].isReadOnly = IsPageReadOnly(i);
    }

    char buffer[64];
    sprintf(buffer, "nachos.swap.%d.%p", kernel->currentThread->processID, this);
    swapFileName = new char[strlen(buffer) + 1];
    strcpy(swapFileName, buffer);
    kernel->fileSystem->Create(swapFileName);
    swapFile = kernel->fileSystem->Open(swapFileName);
    return;
}

//----------------------------------------------------------------------
// AddrSpace::Execute
// 	Run a user program using the current thread
//
//      The program is assumed to have already been loaded into
//      the address space
//
//----------------------------------------------------------------------

void AddrSpace::Execute() {
    kernel->currentThread->space = this;

    this->InitRegisters();  // set the initial register values
    this->RestoreState();   // load page table register

    kernel->machine->Run();  // jump to the user progam

    ASSERTNOTREACHED();  // machine->Run never returns;
                         // the address space exits
                         // by doing the syscall "exit"
}

//----------------------------------------------------------------------
// AddrSpace::InitRegisters
// 	Set the initial values for the user-level register set.
//
// 	We write these directly into the "machine" registers, so
//	that we can immediately jump to user code.  Note that these
//	will be saved/restored into the currentThread->userRegisters
//	when this thread is context switched out.
//----------------------------------------------------------------------

void AddrSpace::InitRegisters() {
    Machine *machine = kernel->machine;
    int i;

    for (i = 0; i < NumTotalRegs; i++) machine->WriteRegister(i, 0);

    // Initial program counter -- must be location of "Start", which
    //  is assumed to be virtual address zero
    machine->WriteRegister(PCReg, 0);

    // Need to also tell MIPS where next instruction is, because
    // of branch delay possibility
    // Since instructions occupy four bytes each, the next instruction
    // after start will be at virtual address four.
    machine->WriteRegister(NextPCReg, 4);

    // Set the stack register to the end of the address space, where we
    // allocated the stack; but subtract off a bit, to make sure we don't
    // accidentally reference off the end!
    machine->WriteRegister(StackReg, numPages * PageSize - 16);
    DEBUG(dbgAddr, "Initializing stack pointer: " << numPages * PageSize - 16);
}

//----------------------------------------------------------------------
// AddrSpace::SaveState
// 	On a context switch, save any machine state, specific
//	to this address space, that needs saving.
//
//	For now, don't need to save anything!
//----------------------------------------------------------------------

void AddrSpace::SaveState() {}

//----------------------------------------------------------------------
// AddrSpace::RestoreState
// 	On a context switch, restore the machine state so that
//	this address space can run.
//
//      For now, tell the machine where to find the page table.
//----------------------------------------------------------------------

void AddrSpace::RestoreState() {
    if (kernel->machine->tlb != NULL) {
        FlushTlb();
        kernel->machine->pageTable = NULL;
        kernel->machine->pageTableSize = 0;
    } else {
        kernel->machine->pageTable = pageTable;
        kernel->machine->pageTableSize = numPages;
    }
}

//----------------------------------------------------------------------
// AddrSpace::Translate
//  Translate the virtual address in _vaddr_ to a physical address
//  and store the physical address in _paddr_.
//  The flag _isReadWrite_ is false (0) for read-only access; true (1)
//  for read-write access.
//  Return any exceptions caused by the address translation.
//----------------------------------------------------------------------
ExceptionType AddrSpace::Translate(unsigned int vaddr, unsigned int *paddr,
                                   int isReadWrite) {
    TranslationEntry *pte;
    int pfn;
    unsigned int vpn = vaddr / PageSize;
    unsigned int offset = vaddr % PageSize;

    if (vpn >= numPages) {
        return AddressErrorException;
    }

    pte = &pageTable[vpn];

    if (!pte->valid) {
        return PageFaultException;
    }

    if (isReadWrite && pte->readOnly) {
        return ReadOnlyException;
    }

    pfn = pte->physicalPage;

    // if the pageFrame is too big, there is something really wrong!
    // An invalid translation was loaded into the page table or TLB.
    if (pfn >= NumPhysPages) {
        DEBUG(dbgAddr, "Illegal physical page " << pfn);
        return BusErrorException;
    }

    pte->use = TRUE;  // set the use, dirty bits

    if (isReadWrite) pte->dirty = TRUE;

    *paddr = pfn * PageSize + offset;

    ASSERT((*paddr < MemorySize));

    // cerr << " -- AddrSpace::Translate(): vaddr: " << vaddr <<
    //  ", paddr: " << *paddr << "\n";

    return NoException;
}

bool AddrSpace::HandlePageFault(int badVAddr) {
    if (badVAddr < 0) return false;
    unsigned int vpn = ((unsigned int)badVAddr) / PageSize;

    if (vpn >= numPages) return false;
    if (pageTable[vpn].valid) {
        InstallTlbEntry(vpn);
        return true;
    }
    return LoadPage(vpn);
}

bool AddrSpace::LoadPage(unsigned int vpn) {
    kernel->addrLock->P();

    if (pageTable[vpn].valid) {
        kernel->addrLock->V();
        return true;
    }

    int physicalPage = AllocateFrame(this, vpn);
    ASSERT(physicalPage >= 0);

    bzero(&(kernel->machine->mainMemory[physicalPage * PageSize]), PageSize);

    if (pageInfo[vpn].hasSwapCopy) {
        LoadFromSwap(vpn, physicalPage);
    } else {
        LoadSegmentPage(noffHeader.code, vpn, physicalPage);
#ifdef RDATA
        LoadSegmentPage(noffHeader.readonlyData, vpn, physicalPage);
#endif
        LoadSegmentPage(noffHeader.initData, vpn, physicalPage);
    }

    pageTable[vpn].physicalPage = physicalPage;
    pageTable[vpn].valid = TRUE;
    pageTable[vpn].use = FALSE;
    pageTable[vpn].dirty = FALSE;
    pageTable[vpn].readOnly = pageInfo[vpn].isReadOnly &&
                              !pageInfo[vpn].hasSwapCopy;

    gFrameTable[physicalPage].occupied = true;
    gFrameTable[physicalPage].owner = this;
    gFrameTable[physicalPage].vpn = vpn;

    InstallTlbEntry(vpn);

    kernel->addrLock->V();
    return true;
}

void AddrSpace::LoadSegmentPage(const Segment &segment, unsigned int vpn,
                                int physicalPage) {
    if (segment.size == 0) return;

    unsigned int pageStart = vpn * PageSize;
    unsigned int pageEnd = pageStart + PageSize;
    unsigned int segmentStart = segment.virtualAddr;
    unsigned int segmentEnd = segment.virtualAddr + segment.size;

    if (pageEnd <= segmentStart || pageStart >= segmentEnd) return;

    unsigned int copyStart =
        (pageStart > segmentStart) ? pageStart : segmentStart;
    unsigned int copyEnd = (pageEnd < segmentEnd) ? pageEnd : segmentEnd;
    unsigned int copySize = copyEnd - copyStart;
    int inFileOffset = segment.inFileAddr + (copyStart - segmentStart);
    char *destination = &(kernel->machine->mainMemory[physicalPage * PageSize +
                                                      (copyStart - pageStart)]);

    executable->ReadAt(destination, copySize, inFileOffset);
}

void AddrSpace::LoadFromSwap(unsigned int vpn, int physicalPage) {
    ASSERT(swapFile != NULL);
    swapFile->ReadAt(&kernel->machine->mainMemory[physicalPage * PageSize],
                     PageSize, SwapOffset(vpn));
}

void AddrSpace::EvictPage(unsigned int vpn) {
    SyncTlbForVpn(vpn);

    TranslationEntry *pte = &pageTable[vpn];
    int physicalPage = pte->physicalPage;
    if (physicalPage < 0) return;

    if (pte->dirty) {
        ASSERT(swapFile != NULL);
        swapFile->WriteAt(&kernel->machine->mainMemory[physicalPage * PageSize],
                          PageSize, SwapOffset(vpn));
        pageInfo[vpn].hasSwapCopy = true;
    }

    InvalidateTlbForPhysicalPage(physicalPage);
    pte->valid = FALSE;
    pte->physicalPage = -1;
    pte->use = FALSE;
    pte->dirty = FALSE;
    pte->readOnly = pageInfo[vpn].isReadOnly && !pageInfo[vpn].hasSwapCopy;
}

bool AddrSpace::IsPageReadOnly(unsigned int vpn) {
    bool overlapsWritable =
        PageOverlapsSegment(noffHeader.initData, vpn) ||
        PageOverlapsSegment(noffHeader.uninitData, vpn);
    if (overlapsWritable) return false;

    if (PageOverlapsSegment(noffHeader.code, vpn)) return true;
#ifdef RDATA
    if (PageOverlapsSegment(noffHeader.readonlyData, vpn)) return true;
#endif
    return false;
}

bool AddrSpace::PageOverlapsSegment(const Segment &segment, unsigned int vpn) {
    if (segment.size <= 0) return false;

    unsigned int pageStart = vpn * PageSize;
    unsigned int pageEnd = pageStart + PageSize;
    unsigned int segmentStart = segment.virtualAddr;
    unsigned int segmentEnd = segment.virtualAddr + segment.size;

    return !(pageEnd <= segmentStart || pageStart >= segmentEnd);
}

int AddrSpace::SwapOffset(unsigned int vpn) { return vpn * PageSize; }

void AddrSpace::FlushTlb() {
    if (kernel->machine->tlb == NULL) return;

    for (int i = 0; i < TLBSize; i++) {
        if (kernel->machine->tlb[i].valid) {
            unsigned int vpn = kernel->machine->tlb[i].virtualPage;
            if (vpn < numPages) {
                pageTable[vpn].use =
                    pageTable[vpn].use || kernel->machine->tlb[i].use;
                pageTable[vpn].dirty =
                    pageTable[vpn].dirty || kernel->machine->tlb[i].dirty;
            }
            kernel->machine->tlb[i].valid = FALSE;
        }
    }
}

void AddrSpace::SyncTlbForVpn(unsigned int vpn) {
    if (kernel->machine->tlb == NULL) return;

    for (int i = 0; i < TLBSize; i++) {
        if (kernel->machine->tlb[i].valid &&
            kernel->machine->tlb[i].virtualPage == (int)vpn) {
            pageTable[vpn].use = pageTable[vpn].use || kernel->machine->tlb[i].use;
            pageTable[vpn].dirty =
                pageTable[vpn].dirty || kernel->machine->tlb[i].dirty;
            kernel->machine->tlb[i].valid = FALSE;
        }
    }
}

void AddrSpace::InvalidateTlbForPhysicalPage(int physicalPage) {
    if (kernel->machine->tlb == NULL) return;

    for (int i = 0; i < TLBSize; i++) {
        if (kernel->machine->tlb[i].valid &&
            kernel->machine->tlb[i].physicalPage == physicalPage) {
            unsigned int vpn = kernel->machine->tlb[i].virtualPage;
            if (vpn < numPages) {
                pageTable[vpn].use = pageTable[vpn].use || kernel->machine->tlb[i].use;
                pageTable[vpn].dirty =
                    pageTable[vpn].dirty || kernel->machine->tlb[i].dirty;
            }
            kernel->machine->tlb[i].valid = FALSE;
        }
    }
}

void AddrSpace::InstallTlbEntry(unsigned int vpn) {
    if (kernel->machine->tlb == NULL) return;

    int slot = gTlbHand % TLBSize;
    gTlbHand = (gTlbHand + 1) % TLBSize;

    if (kernel->machine->tlb[slot].valid) {
        unsigned int oldVpn = kernel->machine->tlb[slot].virtualPage;
        if (oldVpn < numPages) {
            pageTable[oldVpn].use = pageTable[oldVpn].use || kernel->machine->tlb[slot].use;
            pageTable[oldVpn].dirty =
                pageTable[oldVpn].dirty || kernel->machine->tlb[slot].dirty;
        }
    }

    kernel->machine->tlb[slot] = pageTable[vpn];
    kernel->machine->tlb[slot].valid = TRUE;
}

int AllocateFrame(AddrSpace *space, unsigned int vpn) {
    int physicalPage = kernel->gPhysPageBitMap->FindAndSet();
    if (physicalPage != -1) return physicalPage;

    physicalPage = SelectVictimFrame();
    ASSERT(physicalPage >= 0);
    ASSERT(gFrameTable[physicalPage].occupied);
    ASSERT(gFrameTable[physicalPage].owner != NULL);

    gFrameTable[physicalPage].owner->EvictPage(gFrameTable[physicalPage].vpn);
    return physicalPage;
}

int SelectVictimFrame() {
    for (;;) {
        FrameRecord *record = &gFrameTable[gClockHand];

        if (!record->occupied) {
            int frame = gClockHand;
            gClockHand = (gClockHand + 1) % NumPhysPages;
            return frame;
        }

        TranslationEntry *pte =
            &record->owner->pageTable[record->vpn];
        record->owner->SyncTlbForVpn(record->vpn);

        if (!pte->use) {
            int victim = gClockHand;
            gClockHand = (gClockHand + 1) % NumPhysPages;
            return victim;
        }

        pte->use = FALSE;
        gClockHand = (gClockHand + 1) % NumPhysPages;
    }
}
