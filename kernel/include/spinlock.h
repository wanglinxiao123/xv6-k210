#ifndef __SPINLOCK_H
#define __SPINLOCK_H

struct cpu;

struct spinlock
{
  uint locked;     // 是否上锁
  char *name;      // 自旋锁名称
  struct cpu *cpu; // 占有自旋锁的 cpu
};

// Initialize a spinlock
void initlock(struct spinlock *, char *);

// Acquire the spinlock
// Must be used with release()
void acquire(struct spinlock *);

// Release the spinlock
// Must be used with acquire()
void release(struct spinlock *);

// Check whether this cpu is holding the lock
// Interrupts must be off
int holding(struct spinlock *);

#endif
