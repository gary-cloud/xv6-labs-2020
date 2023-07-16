// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

#ifdef LAB_LOCK
struct {
  struct spinlock lock;
  struct run *freelist;
  // 核心思想是：通过用多把锁代替单把锁，消除大部分竞态，减少多个线程在获取单把锁上等待
} kmem[NCPU];
#else
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
#endif

#ifdef LAB_LOCK
void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    char str[10];
	  snprintf(str, 9, "kmem_%d", i);
	  initlock(&kmem[i].lock, str);
  }
  freerange(end, (void*)PHYSTOP);
}
#else
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}
#endif

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

void
mem_steal(int stealer)
{
  int victim = -1;

  for (int i = 0; i < NCPU; i++) {
    if (i == stealer) continue;

    acquire(&kmem[i].lock);
    if (kmem[i].freelist) {
      victim = i;
      break;
    } else {
      release(&kmem[i].lock);
    }
  }

  if (victim == -1) return;     // 无空闲内存

  int freecnt, stealcnt;        // 空闲的内存页数 和 欲盗取的内存页数
  freecnt = 0;
  struct run *run = kmem[victim].freelist;
  while (run != 0) {            // 遍历得到空闲的内存页数
    freecnt++;
    run = run->next;
  }
  // 欲盗取的内存页数 为 空闲的内存页数 的一半
  // 注意边界处理（总不能一页都不偷吧）
  stealcnt = ((freecnt == 1)? 1: (freecnt / 2));

  struct run *temp;
  while (stealcnt--) {
    temp = kmem[stealer].freelist;
    kmem[stealer].freelist = kmem[victim].freelist;
    kmem[victim].freelist = kmem[victim].freelist->next;
    kmem[stealer].freelist->next = temp;
  }

  release(&kmem[victim].lock);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
#ifdef LAB_LOCK
void
kfree(void *pa)
{
  struct run *r;
  int cpu_id;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  cpu_id = cpuid();

  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
}
#else
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}
#endif

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
#ifdef LAB_LOCK
void *
kalloc(void)
{
  struct run *r;
  int cpu_id = cpuid();

  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(!r) {
    mem_steal(cpu_id);           // 若当前cpu无空闲内存，则去盗取其他cpu的
    r = kmem[cpu_id].freelist;
  }

  if(r)
    kmem[cpu_id].freelist = r->next;
  release(&kmem[cpu_id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  return (void*)r;
}
#else
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
#endif
