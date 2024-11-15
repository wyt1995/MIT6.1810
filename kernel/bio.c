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
#define HASH(dev, block) ((dev * 31 + block) % NBUCKET)

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf heads[NBUCKET];
  struct spinlock hashlock[NBUCKET];
} bcache;


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

  // Create hash table of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->prev = &bcache.heads[0];
    b->next = bcache.heads[0].next;
    bcache.heads[0].next->prev = b;
    bcache.heads[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint h = HASH(dev, blockno);

  // Is the block already cached?
  acquire(&bcache.hashlock[h]);
  for (b = bcache.heads[h].next; b != &bcache.heads[h]; b = b->next) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hashlock[h]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.hashlock[h]);

  // Not cached.
  // Recycle unused buffer in other buckets.
  acquire(&bcache.lock);
  int bucket = -1;
  for (int i = 0; i < NBUCKET; i++) {
    acquire(&bcache.hashlock[i]);
    for (b = bcache.heads[i].prev; b != &bcache.heads[i]; b = b->prev) {
      if (b->refcnt == 0) {
        b->prev->next = b->next;
        b->next->prev = b->prev;
        bucket = i;
        break;
      }
    }
    // hold lock if buffer is found
    if (bucket == -1) {
      release(&bcache.hashlock[i]);
    } else {
      break;
    }
  }

  if (bucket == -1)
    panic("bget: no buffers");

  if (bucket != h) {
    release(&bcache.hashlock[bucket]);
    acquire(&bcache.hashlock[h]);
  }

  // move to the hash bucket
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;

  b->prev = &bcache.heads[h];
  b->next = bcache.heads[h].next;
  bcache.heads[h].next->prev = b;
  bcache.heads[h].next = b;

  release(&bcache.hashlock[h]);
  release(&bcache.lock);

  acquiresleep(&b->lock);
  return b;
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

  uint h = HASH(b->dev, b->blockno);
  acquire(&bcache.hashlock[h]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.heads[h].next;
    b->prev = &bcache.heads[h];
    bcache.heads[h].next->prev = b;
    bcache.heads[h].next = b;
  }
  release(&bcache.hashlock[h]);
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

