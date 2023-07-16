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

#ifdef LAB_LOCK
extern uint ticks;    // 当前时间戳
// ticks in trap.c

#define HASH(x) (x % BUCKETNUM)

// 测试时使用大锁，保证除并发部分以外的其他功能正常
struct spinlock big_lock;

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
} bcache;

struct bucket {
  struct spinlock lock;
  struct buf *head;
} buckets[BUCKETNUM];   // 哈希桶：拉链法 或 固定槽位 以解决冲突的哈希表

// 向哈希桶中插入一个元素（头插法）
void buckets_insert(struct buf *item) {
  int hash = HASH(item->blockno);

  item->next = buckets[hash].head;
  buckets[hash].head = item;
}

// 移除哈希桶一个元素
void buckets_remove(struct buf *item) {
  struct buf *b;
  int hash = HASH(item->blockno);

  if (buckets[hash].head == 0) return;

  if (buckets[hash].head == item) {
    buckets[hash].head = item->next;
    return;
  }

  for(b = buckets[hash].head; b->next != 0; b = b->next){
    if (b->next == item) {
      b->next = item->next;
      return;
    }
  }
}
#else
struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;
#endif

#ifdef LAB_LOCK
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for (int i = 0; i < BUCKETNUM; i++) {
    initlock(&buckets[i].lock, "bucket");
    buckets[i].head = 0;
  }
  
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->next = 0;
  }

  initlock(&big_lock, "big_lock");
}
#else
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}
#endif

#ifdef LAB_LOCK
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int hash_in;
  int hash_out;
  hash_in = HASH(blockno);

  acquire(&buckets[hash_in].lock);
  // acquire(&big_lock);

  // Is the block already cached?
  for(b = buckets[hash_in].head; b != 0; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&buckets[hash_in].lock);
      // release(&big_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&buckets[hash_in].lock);

  // Not cached.
  // To find the least ticks of bcache in the loop.
  acquire(&bcache.lock);
  struct buf *recycle = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++) {
    if (b->refcnt == 0) {
      if (recycle == 0) {
	    	recycle = b;
	    	continue;
	    }
      if (b->ticks < recycle->ticks) {
        recycle = b;
      }
    }
  }

  if (recycle == 0) panic("bget: no buffers");

  // Recycle the least recently used (LRU) unused buffer.

  // 1. 在buckets中删除将被移除的buf的索引
  hash_out = HASH(recycle->blockno);
  // 新的块可能哈希到与旧的块相同的桶，确保在这种情况下避免死锁
  // 在这里我避免了锁的嵌套
  // if (hash_out != hash_in)
  acquire(&buckets[hash_out].lock);
  buckets_remove(recycle);
  // if (hash_out != hash_in)
  release(&buckets[hash_out].lock);

  // 2. 初始化重用的buf
  recycle->dev = dev;
  recycle->blockno = blockno;
  recycle->valid = 0;
  recycle->refcnt = 1;
  recycle->ticks = ticks;
  release(&bcache.lock);

  // 3. 在buckets中增加将被移入的buf的索引
  acquire(&buckets[hash_in].lock);
  buckets_insert(recycle);
  release(&buckets[hash_in].lock);

  // release(&big_lock);

  acquiresleep(&recycle->lock);

  return recycle;
}
#else
// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}
#endif

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

#ifdef LAB_LOCK
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  
  int hash = HASH(b->blockno);
  acquire(&buckets[hash].lock);
  // acquire(&big_lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
	  b->ticks = ticks;
  }
  release(&buckets[hash].lock);
  // release(&big_lock);
}
#else
// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}
#endif

#ifdef LAB_LOCK
void
bpin(struct buf *b) {
  int hash = HASH(b->blockno);
  acquire(&buckets[hash].lock);
  // acquire(&big_lock);
  b->refcnt++;
  release(&buckets[hash].lock);
  // release(&big_lock);
}
#else
void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}
#endif

#ifdef LAB_LOCK
void
bunpin(struct buf *b) {
  int hash = HASH(b->blockno);
  acquire(&buckets[hash].lock);
  // acquire(&big_lock);
  b->refcnt--;
  release(&buckets[hash].lock);
  // release(&big_lock);
}
#else
void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}
#endif


