// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

int HASH(int id) {
    return id % NBUCKET;
}

extern uint ticks;

struct {
    // bcache 缓冲区的锁
    struct spinlock block;
    // 对应每个散列桶的锁
    struct spinlock lock[NBUCKET];
    // 缓冲区
    struct buf buf[NBUF];

    // Linked list of all buffers, through prev/next.
    // Sorted by how recently the buffer was used.
    // head.next is most recent, head.prev is least.
    // 缓冲区双向链表的头节点
    struct buf head[NBUCKET];
} bcache;

void
binit(void) {
    struct buf *b;
    char lockname[16];

    initlock(&bcache.block, "bcache_big_lock");

    for (int i = 0; i < NBUCKET; i++) {
        // 为每一个锁取名
        snprintf(lockname, sizeof(lockname), "bcache_%d", i);
        initlock(&bcache.lock[i], lockname);

        // 初始化每个桶的头节点
        bcache.head[i].prev = &bcache.head[i];
        bcache.head[i].next = &bcache.head[i];
    }

    // Create linked list of buffers
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        // 头插法插入散列桶 0 上的缓冲区链表
        b->next = bcache.head[0].next;
        b->prev = &bcache.head[0];
        bcache.head[0].next->prev = b;
        bcache.head[0].next = b;
        // 初始化睡眠锁
        initsleeplock(&b->lock, "buffer");
    }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno) {
    struct buf *b;

    int bid = HASH(blockno);

    acquire(&bcache.lock[bid]);

    // Is the block already cached?
    // 1、判断缓存是否命中
    for (b = bcache.head[bid].next; b != &bcache.head[bid]; b = b->next) {
        // 从头到尾遍历缓冲块 判断缓存区中是否缓存指定设备号上指定物理块号的物理块
        if (b->dev == dev && b->blockno == blockno) {
            // 引用次数加一
            b->refcnt++;
            release(&bcache.lock[bid]);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // 2.1 释放散列桶的锁 为之后获取缓冲区的锁做准备
    release(&bcache.lock[bid]);

    // 2.2 先获取缓冲区的锁 在获取散列桶的锁 避免死锁
    acquire(&bcache.block);
    acquire(&bcache.lock[bid]);

    // Not cached.
    // Recycle the least recently used (LRU) unused buffer.
    // 2.3 缓存未命中
    // 2.3.1 从当前散列桶中查找目标缓存块
    // 由于可能存在在 2.1 之后其他进程也缓存了物理块进而导致重复缓存
    // 因此需要重新查找一遍缓存链表
    for (b = bcache.head[bid].next; b != &bcache.head[bid]; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bcache.lock[bid]);
            release(&bcache.block);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // 2.3.2 从当前散列桶中查找可以替换的缓冲块
    uint min_ticks = 0;
    struct buf *p = 0;
    for (b = bcache.head[bid].next; b != &bcache.head[bid]; b = b->next) {
        // 这里需要根据 ticks 选择最近最少使用的缓冲块
        if (b->refcnt == 0 && (b->mticks < min_ticks || p == 0)) {
            // 2.3.2 找到比先前的空闲缓冲块使用更早的
            min_ticks = b->mticks;
            p = b;
        }
    }
    // 2.3.3 有满足条件的缓冲块 返回
    if (p) {
        p->dev = dev;
        p->blockno = blockno;
        p->refcnt++;
        p->valid = 0;
        release(&bcache.lock[bid]);
        release(&bcache.block);
        acquiresleep(&p->lock);
        return p;
    }

    // 2.3.4 不存在满足的缓冲块
    // 即当前散列桶没有空闲的缓冲块 则需要从其他散列桶中窃取
    for (int i = HASH(bid + 1); i != bid; i = HASH(i + 1)) {
        acquire(&bcache.lock[i]);
        for (b = bcache.head[i].next; b != &bcache.head[i]; b = b->next) {
            if (b->refcnt == 0 && (p == 0 || b->mticks < min_ticks)) {
                min_ticks = b->mticks;
                p = b;
            }
        }
        if (p) {
            p->dev = dev;
            p->blockno = blockno;
            p->refcnt++;
            p->valid = 0;
            // 从原先的散列桶中移除
            p->next->prev = p->prev;
            p->prev->next = p->next;
            release(&bcache.lock[i]);
            // 将移除出来的缓冲块添加到当前散列桶中
            p->next = bcache.head[bid].next;
            p->prev = &bcache.head[bid];
            bcache.head[bid].next->prev = p;
            bcache.head[bid].next = p;
            release(&bcache.lock[bid]);
            release(&bcache.block);
            acquiresleep(&p->lock);
            return p;
        }
        release(&bcache.lock[i]);
    }
    release(&bcache.lock[bid]);
    release(&bcache.block);
    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
// 返回一个带睡眠锁的缓冲块
struct buf *
bread(uint dev, uint blockno) {
    struct buf *b;

    // dev 设备号 blockno 设备上的物理块号
    b = bget(dev, blockno);
    // 如果缓冲区不包含该物理块的副本
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b) {
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b) {
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock);

    // 根据缓存块缓存的物理块号计算需要操作哪个散列桶
    int bid = HASH(b->blockno);

    // 获取该散列桶的锁
    acquire(&bcache.lock[bid]);
    // 引用次数减一
    b->refcnt--;
    // 如果引用次数为 0
    if (b->refcnt == 0) {
//        // no one is waiting for it.
//        // 将该缓冲块从缓冲区链表中断开
//        b->next->prev = b->prev;
//        b->prev->next = b->next;
//        // 将断开的缓冲块插入到缓冲区链表头部
//        b->next = bcache.head[bid].next;
//        b->prev = &bcache.head[bid];
//        bcache.head.next->prev = b;
//        bcache.head.next = b;
        acquire(&tickslock);
        b->mticks = ticks;
        release(&tickslock);
    }

    // 释放该散列桶的锁
    release(&bcache.lock[bid]);
}

void
bpin(struct buf *b) {
    int bid = HASH(b->blockno);
    acquire(&bcache.lock[bid]);
    b->refcnt++;
    release(&bcache.lock[bid]);
}

void
bunpin(struct buf *b) {
    int bid = HASH(b->blockno);
    acquire(&bcache.lock[bid]);
    b->refcnt--;
    release(&bcache.lock[bid]);
}


