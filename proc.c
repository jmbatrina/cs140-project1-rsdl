#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// Phase 1-2: Force RSDL_LEVELS to 1 and RSDL_STARTING_LEVEL to 0
#undef RSDL_LEVELS
#define RSDL_LEVELS 1
#undef RSDL_STARTING_LEVEL
#define RSDL_STARTING_LEVEL 0

#define NULL (void *) 0x0

// TODO: check if we need to release ptable.lock before each (debug) panic
// TODO: Add tests for en/unqueue (automated testcases????)

// NOTE: each level is represented as an array with NPROC elements
//       for simplicity (since the previous linke list approach had a lot of mysterious crashes)
// TODO: switch to circular array representation so that dequeue of front (common case) is O(1)
struct level_queue{
  struct spinlock lock;
  // must only be modified by enqueue_proc and unqueue_proc
  int numproc;
  struct proc *proc[NPROC];
};

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
      cprintf("%d|%s|0(0)", ticks, set_name);
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

  if (p->state == UNUSED) {
    panic("enqueue of UNUSED proc");
    return;
  }

  if (q == NULL) {
    panic("enqueue in NULL queue");
    return;
  }

  if (q->numproc < 0) {
    panic("unqueue on queue with NEGATIVE numproc");
    return;
  }

  if (q->numproc > NPROC) {
    panic("unqueue on queue with numproc > NPROC");
    return;
  }

  acquire(&q->lock);
  // for debug, see below
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
  if (p == NULL) {
    panic("unqueue of NULL proc");
    return -1;
  }

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

  if (q->numproc < 0) {
    panic("unqueue on queue with NEGATIVE numproc");
    return -1;
  }

  if (q->numproc > NPROC) {
    panic("unqueue on queue with numproc > NPROC");
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
  // TODO: More efficient implementation (keep current level as proc attribute, needs to be maintained when proc is re-enqueued)
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
    panic("Proc not found in any level");
    return -1;
  }

  return 0;
}

int
next_level(int start, int use_expired)
{
  // TODO: ptable.{active, expired} accessed but not modified. Do we need to acquire lock?
  const struct level_queue *set = (use_expired) ? ptable.expired : ptable.active;

  int k = start;
  for ( ; k < RSDL_LEVELS; ++k) {
    if (set[k].numproc < NPROC) {
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
find_vacant_queue(int start)
{
  int level = next_active_level(start);
  struct level_queue *set = ptable.active;
  if (level == -1) {
    level = next_expired_level(RSDL_STARTING_LEVEL);
    set = ptable.expired;
  }

  if (level == -1) {
    // TODO: Verify how to handle case when all levels are filled
    panic("No free level in expired and active set, too many procs");
    return NULL;
  }

  return &set[level];
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
  struct level_queue *q = find_vacant_queue(RSDL_STARTING_LEVEL);
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
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

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

  np->state = RUNNABLE;
   // only enqueue here since we are sure that allocation is successful
  struct level_queue *q = find_vacant_queue(RSDL_STARTING_LEVEL);
  enqueue_proc(np, q);

  release(&ptable.lock);

  return pid;
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
    struct level_queue *nq;
    for (k = 0; k < RSDL_LEVELS; ++k) {
      q = &ptable.active[k];
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
      switchuvm(p);
      p->state = RUNNING;

      if (schedlog_active && ticks <= schedlog_lasttick) {
        print_schedlog();
      }

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // proc has given up control to scheduler. Check if we need
      // to replenish quantum or move to lower priority queue
      if (p->ticks_left == 0) {
        // proc used up quantum: enqueue to lower priority
        p->ticks_left = RSDL_PROC_QUANTUM;
        nk = k + 1;
      } else {
        // proc yielded with remaining quantum: re-enqueue to same level
        nk = k;
      }

      // only try to re-enqueue proc if it was not removed before
      // e.g. when it calls exit() (state == ZOMBIE), it removes itself so no need to re-enqueue
      if (q->numproc != 0 && p->state != ZOMBIE) {
        prev_idx = unqueue_proc(p, q);
        if (prev_idx == -1) {
          panic("re-enqueue of proc failed");
        }
        nq = find_vacant_queue(nk);
        if (is_expired_set(nq)) {
          // proc quantum refresh case 2: proc moved to expired set
          p->ticks_left = RSDL_PROC_QUANTUM;
        }
        enqueue_proc(p, nq);
      }

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    } else {
      // No RUNNABLE proc found; Can happen before initcode runs, all procs sleeping but will return after n ms, etc.
      // Since there are no procs ready in active set, we swap sets
      nq = ptable.active;
      ptable.active = ptable.expired;
      ptable.expired = nq;

      // re-enqueue procs in old active set (expired set) to new active set
      for (k = 0; k < RSDL_LEVELS; ++k) {
        q = &ptable.expired[k];
        while (q->numproc != 0) {
          p = q->proc[0];
          // proc will be re-enqueued to new level, replenish quantum
          p->ticks_left = RSDL_PROC_QUANTUM;
          unqueue_proc(p, q);

          nq = find_vacant_queue(RSDL_STARTING_LEVEL);
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
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
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
