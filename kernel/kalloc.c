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

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

// 参考 memlayout.h
uint ref[PHYSTOP / PGSIZE];

void
kinit() {
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *) PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end) {
  char *p;
  p = (char *) PGROUNDUP((uint64) pa_start);
  for (; p + PGSIZE <= (char *) pa_end; p += PGSIZE) {
    ref[(uint64) p / PGSIZE] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa) {
  struct run *r;

  if (((uint64) pa % PGSIZE) != 0 || (char *) pa < end || (uint64) pa >= PHYSTOP)
    panic("kfree");

  // 当引用计数为 0 时回收空间
  if (--ref[(uint64) pa / PGSIZE] == 0) {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *) pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r) {
    memset((char *) r, 5, PGSIZE); // fill with junk
    // 将引用计数初始化为 1
    ref[(uint64) r / PGSIZE] = 1;
  }
  return (void *) r;
}

/**
 * 判断页面是否为 COW 页面
 * @param pagetable 页表
 * @param va 虚拟地址
 * @return 0 是 -1 不是
 */
int
cowpage(pagetable_t pagetable, uint64 va) {
  if (va >= MAXVA)
    return -1;
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
    return -1;
  if ((*pte & PTE_V) == 0)
    return -1;
  return (*pte & PTE_F ? 0 : -1);
}

/**
 * copy on write 写时复制分配
 * @param pagetable 页表
 * @param va 虚拟地址
 * @return va 对应的新物理地址
 */
void *
cowalloc(pagetable_t pagetable, uint64 va) {
  va = PGROUNDDOWN(va);
  // 能进来的一定时 cow 页面
  uint64 pa, mem = 0;
  pte_t *pte;

  // 获取对应物理地址
  if ((pa = walkaddr(pagetable, va)) == 0) return 0;
  // 获取对应页表项
  if ((pte = walk(pagetable, va, 0)) == 0) return 0;
  // 如果是 COW 页面
  if (ref[pa / PGSIZE] == 1) {
    // 只剩一个进程对此物理地址存在引用
    // 则直接修改对应的 PTE 即可
    *pte |= PTE_W;
    *pte &= ~PTE_F;
    return (void *) pa;
  } else {
    if ((mem = (uint64) kalloc()) == 0) return 0;
    // 将旧页面的数据复制到新页面
    memmove((char *) mem, (char *) pa, PGSIZE);
    // 清除 PTE_V 标志位 否则在 mappagges 中会判定为 remap
    *pte &= ~PTE_V;
    if (mappages(pagetable, va, PGSIZE, mem, (PTE_FLAGS(*pte) | PTE_W) & ~PTE_F) != 0) {
      kfree((void *) mem);
      *pte |= PTE_V;
      return 0;
    }
  }
  // 旧页面地址引用计数减一
  kfree((void *) PGROUNDDOWN(pa));
  return (void *) mem;
}
