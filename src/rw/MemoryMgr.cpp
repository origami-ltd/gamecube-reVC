#include "common.h"
#include "MemoryHeap.h"
#include "MemoryMgr.h"
#ifdef GTA_OGC
extern "C" void *gcBigAlloc(size_t sz);
extern "C" void gcBigFree(void *p);
extern "C" int gcBigContains(const void *p);
extern "C" size_t gcBigSizeOf(const void *p);
extern "C" int gcBigResize(void *p, size_t size);
extern "C" void *gcBigMove(void *p);
static size_t gcMoveBudget;
unsigned gcMemoryMoves, gcMemoryMovedBytes;
#endif


uint8 *pMemoryTop;

extern uint8 _end[];
extern uint8 _stack_size[];

void
InitMemoryMgr(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
#ifdef GTA_PS2
	// not quite clear what the 0x1000s and 0x10 are exactly
	uint32 memUsed = (uint32)_end + (uint32)_stack_size + 0x1000 + 0x1000;
	uint32 heapSize = 32*1024*1024 - memUsed - 0x10;
printf("Heap size: %d\n", heapSize);
	gMainHeap.Init(heapSize);

#else
	// randomly allocate 128mb
	gMainHeap.Init(128*1024*1024);
#endif
#endif
}


RwMemoryFunctions memFuncs = {
	MemoryMgrMalloc,
	MemoryMgrFree,
	MemoryMgrRealloc,
	MemoryMgrCalloc
};

#ifdef USE_CUSTOM_ALLOCATOR
// game seems to be using heap directly here, but this is nicer
void *operator new(size_t sz) throw() { return MemoryMgrMalloc(sz); }
void *operator new[](size_t sz) throw() { return MemoryMgrMalloc(sz); }
void operator delete(void *ptr) throw() { MemoryMgrFree(ptr); }
void operator delete[](void *ptr) throw() { MemoryMgrFree(ptr); }
#endif

void*
MemoryMgrMalloc(size_t size)
{
#ifdef USE_CUSTOM_ALLOCATOR
	void *mem = gMainHeap.Malloc(size);
#elif defined(GTA_OGC)
	void *mem = size >= 1024 ? gcBigAlloc(size) : NULL;   // B79: big RW blocks live in their own chunks (B94: 4K+; B106: 1K+ — B100 declared BIG_MIN=1024 but this threshold stayed 4096, and the 'OOM need 3K with 615K free' deaths were 3-4K RW blocks with no hole in the general heap)
	if(mem == NULL) mem = malloc(size);
#else
	void *mem = malloc(size);
#endif
	if((uint8*)mem + size > pMemoryTop)
		pMemoryTop = (uint8*)mem + size ;
	return mem;
}

void*
MemoryMgrRealloc(void *ptr, size_t size)
{
#ifdef USE_CUSTOM_ALLOCATOR
	void *mem = gMainHeap.Realloc(ptr, size);
#elif defined(GTA_OGC)
	void *mem;
	if(ptr && gcBigContains(ptr)){
		size_t old = gcBigSizeOf(ptr);
		if(size && size <= old && gcBigResize(ptr, size)) return ptr;
		mem = MemoryMgrMalloc(size);
		if(mem){ memcpy(mem, ptr, old < size ? old : size); gcBigFree(ptr); }
	}else if(ptr == NULL) mem = MemoryMgrMalloc(size);
	else mem = realloc(ptr, size);
#else
	void *mem = realloc(ptr, size);
#endif
	if((uint8*)mem + size  > pMemoryTop)
		pMemoryTop = (uint8*)mem + size ;
	return mem;
}

void*
MemoryMgrCalloc(size_t num, size_t size)
{
#ifdef USE_CUSTOM_ALLOCATOR
	void *mem = gMainHeap.Malloc(num*size);
#elif defined(GTA_OGC)
	void *mem = MemoryMgrMalloc(num*size);
	if(mem) memset(mem, 0, num*size);
#else
	void *mem = calloc(num, size);
#endif
	if((uint8*)mem + size  > pMemoryTop)
		pMemoryTop = (uint8*)mem + size ;
#ifdef FIX_BUGS
	if(mem) memset(mem, 0, num*size);
#endif
	return mem;
}

void
MemoryMgrBeginCompaction(size_t maxBytes)
{
#ifdef GTA_OGC
	gcMoveBudget = maxBytes;
#endif
}

void *
MemoryMgrMoveMemory(void *ptr)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return gMainHeap.MoveMemory(ptr);
#elif defined(GTA_OGC)
	size_t size = gcBigSizeOf(ptr);
	if(size && size <= gcMoveBudget){
		void *moved = gcBigMove(ptr);
		if(moved != ptr){
			gcMoveBudget -= size;
			gcMemoryMoves++;
			gcMemoryMovedBytes += size;
		}
		return moved;
	}
#endif
	return ptr;
}

void
MemoryMgrFree(void *ptr)
{
#ifdef USE_CUSTOM_ALLOCATOR
#ifdef FIX_BUGS
	// i don't suppose this is handled by RW?
	if(ptr == nil) return;
#endif
	gMainHeap.Free(ptr);
#elif defined(GTA_OGC)
	if(ptr && gcBigContains(ptr)) gcBigFree(ptr); else free(ptr);
#else
	free(ptr);
#endif
}

void *
RwMallocAlign(RwUInt32 size, RwUInt32 align)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	uintptr ptralign = align-1;
	void *mem = (void *)MemoryMgrMalloc(size + sizeof(uintptr) + ptralign);

	ASSERT(mem != nil);
	// A failed malloc must come back as nil, not as (nil + align): the
	// aligned garbage (0x800 for a sector-aligned request) looked like a valid
	// buffer to the streamer and a DVD read landed on low memory (B18/B19).
	if(mem == nil)
		return nil;

	void *addr = (void *)((((uintptr)mem) + sizeof(uintptr) + ptralign) & ~ptralign);

	ASSERT(addr != nil);
#else
	void *mem = (void *)MemoryMgrMalloc(size + align);

	ASSERT(mem != nil);
	if(mem == nil)
		return nil;

	void *addr = (void *)((((uintptr)mem) + align) & ~(align - 1));

	ASSERT(addr != nil);
#endif

	*(((void **)addr) - 1) = mem;

	return addr;
}

void
RwFreeAlign(void *mem)
{
	ASSERT(mem != nil);

	void *addr = *(((void **)mem) - 1);

	ASSERT(addr != nil);

	MemoryMgrFree(addr);
}
