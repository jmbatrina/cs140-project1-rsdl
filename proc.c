#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define NULL (void *) 0x0

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  // pointers to start of active and expired sets
  // either active = &level[0] and expired &level[1] or vice versa
  // NOTE: Only active[0..RSDL_LEVELS-1] are correct access, ditto for expired
  struct level_queue *active;
  struct level_queue *expired;
  struct level_queue level[2][RSDL_LEVELS];
} ptable;

static struct proc *initproc;

int nextpid = 1;

int
is_active_set(struct level_queue *q)
{
  // NOTE: assumes that &ptable.level[0][0] <= q < &ptable.level[1][RSDL_LEVELS]
  //       since each level queue is created in the contiguous ptable.level
  // q is in active set if its address is within ptable.active
  // otherwise since q is inside ptable.level, then it must be in ptable.expired
  return ptable.active <= q
         && q < &ptable.active[RSDL_LEVELS];
}

int is_expired_set(struct level_queue *q)
{
  return !is_active_set(q);
}

// Variables for scheduling logs. See schedlog() and scheduler() below
int schedlog_active = 0;
int schedlog_lasttick = 0;

void schedlog(int n) {
  schedlog_active = 1;
  schedlog_lasttick = ticks + n;
}

void print_schedlog(void) {
  struct proc *pp;
  struct level_queue *qq;

  struct level_queue *set[] = {ptable.active, ptable.expired};
  for (int s = 0; s < 2; ++s) {
    char *set_name = (is_active_set(&set[s][0])) ? "active" : "expired";
    for (int k = 0; k < RSDL_LEVELS; ++k) {
      qq = &set[s][k];
      acquire(&qq->lock);
      cprintf("%d|%s|%d(%d)", ticks, set_name, k, qq->ticks_left);
      for(int i = 0; i < qq->numproc; ++i) {
        pp = qq->proc[i];
        if (pp->state == UNUSED) continue;
        else cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->ticks_left);
      }
      release(&qq->lock);

      cprintf("\n");
    }
  }
}

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  struct level_queue *lq;
  initlock(&ptable.lock, "ptable");

  // To be sure, explicitly initialize all queues to empty
  acquire(&ptable.lock);
  for (int s = 0; s < 2; ++s) {
    for (int k = 0; k < RSDL_LEVELS; ++k){
      lq = &ptable.level[s][k];
      // NOTE: all queues will have same lock names
      initlock(&lq->lock, "level queue");
      acquire(&lq->lock);
      lq->numproc = 0;
      lq->ticks_left = RSDL_LEVEL_QUANTUM;
      for (int i = 0; i < NPROC; ++i){
        lq->proc[i] = NULL;
      }
      release(&lq->lock);
    }
  }

  // initialize pointers to active and expired sets
  ptable.active = ptable.level[0];
  ptable.expired = ptable.level[1];
  release(&ptable.lock);
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

void
enqueue_proc(struct proc *p, struct level_queue *q)
{
  if (p == NULL) {
    panic("enqueue of NULL proc node");
    return;
  }

  if (q == NULL) {
    panic("enqueue in NULL queue");
    return;
  }

  acquire(&q->lock);
  if (q->numproc >= NPROC) {
    panic("enqueue in full level");
  } else {
    // enqueue *p and increment number of procs in this level
    q->proc[q->numproc++] = p;
  }
  release(&q->lock);
}

// NOTE: *un*queue intentional since proc in middle of queue can be removed
// returns index of proc in current level
int
unqueue_proc_full(struct proc *p, struct level_queue *q, int isTry)
{
  if (q == NULL) {
    panic("unqueue in NULL queue");
    return -1;
  }

  if (q->numproc == 0) {
    if (!isTry) {
      panic("unqueue on empty level");
    }
    return -1;
  }

  int found = 0;
  int i, j;

  acquire(&q->lock);
  for (i = 0; i < q->numproc; ++i) {
    if (q->proc[i] == p) {
      // found proc, remove from current queue linked list
      found = 1;
      break;
    }
  }

  if (found) {
    // move succeeding procs up the queue
    for (j = i+1; j < q->numproc; ++j) {
      q->proc[j-1] = q->proc[j];
    }
    q->numproc--;   // decrement number of procs in this level
  }
  release(&q->lock);

  if (!found) {
    if (!isTry) {
      panic("unqueue of node not belonging to level");
    }
    return -1;
  }

  // we only reach here if unqueue is successful
  return i;
}

int
unqueue_proc(struct proc *p, struct level_queue *q)
{
  return unqueue_proc_full(p, q, 0);
}

int
try_unqueue_proc(struct proc *p, struct level_queue *q)
{
  return unqueue_proc_full(p, q, 1);
}

int
remove_proc_from_levels(struct proc *p)
{
  struct level_queue *q;
  int found = 0;
  // Naive implementation: use linear search on each level to find level
  for (int s = 0; s < 2; ++s) {
    for (int k = 0; k < RSDL_LEVELS; ++k){
      q = &ptable.level[s][k];
      if (try_unqueue_proc(p, q) != -1) {
        found = 1;
        break;
      }
    }
    if (found)
      break;
  }

  if (!found) {
    return -1;
  }

  return 0;
}

int
next_level(int start, int use_expired)
{
  const struct level_queue *set = (use_expired) ? ptable.expired : ptable.active;
  if (start < 0)
    return -1;

  int k = start;
  for ( ; k < RSDL_LEVELS; ++k) {
    if (set[k].ticks_left > 0 && set[k].numproc < NPROC) {
      break;
    }
  }

  if (k < RSDL_LEVELS) {
    return k;
  } else {
    return -1;
  }
}

int
next_active_level(int start)
{
  return next_level(start, 0);
}

int
next_expired_level(int start)
{
  return next_level(start, 1);
}

struct level_queue*
find_available_queue(int active_start, int expired_start)
{
  int level = next_active_level(active_start);
  if (level == -1) {  // no lower prio level available
    // re-enqueue in expired set instead, starting at expired_set
    level = next_expired_level(expired_start);
    if (level == -1) {
      // NOTE: shouldn't happen normally
      panic("No free level in expired and active set, too many procs");
      return NULL;
    }

    // We reach here if we found available queue in expired set
    return &ptable.expired[level];
  }

  // We reach here if we found available queue in active set
  return &ptable.active[level];
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      goto found;
  }

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->ticks_left = RSDL_PROC_QUANTUM;
  p->default_level = RSDL_STARTING_LEVEL;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  // only enqueue here since we are sure that allocation is successful
  struct level_queue *q = find_available_queue(p->default_level, p->default_level);
  enqueue_proc(p, q);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
// Accessible as either fork(void) or priofork(int) syscalls
int
priofork(int default_level)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // default_level too large
  if (default_level >= RSDL_LEVELS) {
    return -1;
  }

  // default_level negative
  if (default_level < 0) {
    return -1;
  }

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->default_level = default_level;  // set priority level
  np->state = RUNNABLE;
   // only enqueue here since we are sure that allocation is successful
  struct level_queue *q = find_available_queue(np->default_level, np->default_level);
  enqueue_proc(np, q);

  release(&ptable.lock);

  return pid;
}

// original fork() call
int
fork(void)
{
  return priofork(RSDL_STARTING_LEVEL);
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Process exited, remove from its queue
  remove_proc_from_levels(curproc);

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);

        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p = NULL;
  struct level_queue *q = NULL;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    int i, prev_idx, k, nk;
    int found = 0;
    struct proc *np;
    struct level_queue *nq;
    for (k = 0; k < RSDL_LEVELS; ++k) {
      q = &ptable.active[k];
      if (q->ticks_left <= 0)
        continue;

      acquire(&q->lock);
      for (i = 0; i < q->numproc; ++i ) {
        p = q->proc[i];
        if(p->state == RUNNABLE && p->ticks_left > 0) {
          found = 1;
          break;
        }
      }
      release(&q->lock);
      if (found)
        break;
    }

    if (schedlog_active && ticks > schedlog_lasttick) {
        schedlog_active = 0;
    }

    if (found) {
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      c->queue = q;
      switchuvm(p);
      p->state = RUNNING;

      if (schedlog_active && ticks <= schedlog_lasttick) {
        print_schedlog();
      }

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // proc has given up control to scheduler
      if (q->ticks_left <= 0) {
        // level-local quantum depleted, migrate all procs
        while (q->numproc > 0) {
          np = q->proc[0];
          // moving to next level OR expired set, replenish quantum
          np->ticks_left = RSDL_PROC_QUANTUM;

          unqueue_proc(np, q);
          // Section 2.4: The active process should be enqueued last
          if (np == p) {
            continue;
          }

          // move proc to next available level in active set
          // if none, enqueue to original level in expired set
          nq = find_available_queue(k+1, np->default_level);
          // re-enqueue to same level but in active set, or below
          enqueue_proc(np, nq);
        }

        // If proc called exit, it already unqueued itself; no need to re-enqueue
        if (p->state != ZOMBIE) {
          // active process is the last process to be enqueued
          nq = find_available_queue(k+1, p->default_level);
          enqueue_proc(p, nq);
        }
      } else {
        // NOTE: if local-level quantum was depleted, procs have already been
        //       replenished and reprioritized, so we only do things below
        //       when the level still has remaining quantum
        // Check if we need to replenish quantum or move to lower priority queue
        if (p->ticks_left <= 0) {
          // proc used up quantum: enqueue to lower priority
          p->ticks_left = RSDL_PROC_QUANTUM;
          nk = k + 1;
        } else {
          // proc yielded with remaining quantum: re-enqueue to same level
          nk = k;
        }

        // only try to re-enqueue proc if it was not removed before
        // e.g. when it calls exit() (state == ZOMBIE), it removes itself so no need to re-enqueue
        if (q->numproc > 0 && p->state != ZOMBIE) {
          prev_idx = unqueue_proc(p, q);
          if (prev_idx == -1) {
            panic("re-enqueue of proc failed");
          }
          // find vacant queue, starting from level nk as decided above
          // if no available level in active set, enqueue to original level in expired set
          nq = find_available_queue(nk, p->default_level);
          if (is_expired_set(nq)) {
            // proc quantum refresh case 2: proc moved to expired set
            p->ticks_left = RSDL_PROC_QUANTUM;
          }
          enqueue_proc(p, nq);
        }
      }

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      c->queue = NULL;
    } else {
      // No RUNNABLE proc found; Can happen before initcode runs, all procs sleeping but will return after n ms, etc.
      // Since there are no procs ready in active set, we swap sets
      nq = ptable.active;
      ptable.active = ptable.expired;
      ptable.expired = nq;

      // re-enqueue procs in old active set (expired set) to new active set
      for (k = 0; k < RSDL_LEVELS; ++k) {
        q = &ptable.expired[k];
        q->ticks_left = RSDL_LEVEL_QUANTUM; // replenish level-local quantum
        while (q->numproc > 0) {
          p = q->proc[0];
          // proc will be re-enqueued to new level, replenish quantum
          p->ticks_left = RSDL_PROC_QUANTUM;
          unqueue_proc(p, q);

          // re-enqueue to original level in active set
          // if no available level in active set, enqueue to original level in expired set
          nk = p->default_level;
          nq = find_available_queue(nk, nk);
          enqueue_proc(p, nq);
        }
      }
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    = "unused",
  [EMBRYO]    = "embryo",
  [SLEEPING]  = "sleep ",
  [RUNNABLE]  = "runble",
  [RUNNING]   = "run   ",
  [ZOMBIE]    = "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = &ptable.proc[0]; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
