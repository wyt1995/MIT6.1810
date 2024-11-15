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

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all free buffers
  struct buf pool;

  // Hash table of buffers in-use
  struct buf heads[NBUCKET];
  struct spinlock hashlock[NBUCKET];
} bcache;

uint
hashcode(uint num) {
  return num % NBUCKET;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.hashlock[i], "bcache_table");

    bcache.heads[i].prev = &bcache.heads[i];
    bcache.heads[i].next = &bcache.heads[i];
  }

  // Create linked list of buffers
  bcache.pool.prev = &bcache.pool;
  bcache.pool.next = &bcache.pool;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.pool.next;
    b->prev = &bcache.pool;
    initsleeplock(&b->lock, "buffer");
    bcache.pool.next->prev = b;
    bcache.pool.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint i = hashcode(blockno);
  acquire(&bcache.hashlock[i]);

  // Is the block already cached?
  for (b = bcache.heads[i].next; b != &bcache.heads[i]; b = b->next) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hashlock[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.hashlock[i]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer from pool.
  acquire(&bcache.lock);
  for (b = bcache.pool.prev; b != &bcache.pool; b = b->prev) {
    if(b->refcnt == 0) {
      b->prev->next = b->next;
      b->next->prev = b->prev;
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);

      acquire(&bcache.hashlock[i]);
      b->prev = &bcache.heads[i];
      b->next = bcache.heads[i].next;
      bcache.heads[i].next->prev = b;
      bcache.heads[i].next = b;
      release(&bcache.hashlock[i]);

      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

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

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint i = hashcode(b->blockno);
  acquire(&bcache.hashlock[i]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    release(&bcache.hashlock[i]);

    acquire(&bcache.lock);
    b->next = bcache.pool.next;
    b->prev = &bcache.pool;
    bcache.pool.next->prev = b;
    bcache.pool.next = b;
    release(&bcache.lock);
  } else {
    release(&bcache.hashlock[i]);
  }
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


