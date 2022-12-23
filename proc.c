#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#include "rsdl.h" // For RSDL scheduler parameters

// Phase 1: Force RSDL_LEVELS to 1 and RSDL_STARTING_LEVEL to 0
#undef RSDL_LEVELS
#define RSDL_LEVELS 1
#undef RSDL_STARTING_LEVEL
#define RSDL_STARTING_LEVEL 0

#define NULL (void *) 0x0

// TODO: check if we need to release ptable.lock before each (debug) panic
// TODO: Add tests for en/unqueue (automated testcases????)

struct level_node {
  struct proc proc;
  // must only be modified by enqueue_node and unqueue_proc
  struct level_node *next;
};

struct level_queue{
  struct spinlock lock;
  // must only be modified by enqueue_node and unqueue_proc
  int numproc;
  struct level_node *head;
};

struct {
  struct spinlock lock;
  struct level_node node[RSDL_LEVELS*NPROC];
  struct level_queue active[RSDL_LEVELS];
} ptable;

static struct proc *initproc;

int nextpid = 1;

// Variables for scheduling logs. See schedlog() and scheduler() below
int schedlog_active = 0;
int schedlog_lasttick = 0;

void schedlog(int n) {
  schedlog_active = 1;
  schedlog_lasttick = ticks + n;
}

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  struct level_node *n;
  struct level_queue *lq;
  initlock(&ptable.lock, "ptable");

  // To be sure, explicitly initialize next pointers to NULL
  acquire(&ptable.lock);
  for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
    n->next = NULL;
  }

  // Explicitly initialize all queues to empty
  for (lq = &ptable.active[0]; lq < &ptable.active[RSDL_LEVELS]; lq++){
    initlock(&lq->lock, "level queue");
    lq->numproc = 0;
    lq->head = NULL;
  }
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
enqueue_node(struct level_node *n, struct level_queue *q)
{
  if (n == NULL) {
    panic("enqueue of NULL proc node");
    return;
  }

  if (n->proc.state == UNUSED) {
    panic("enqueue of UNUSED proc");
    return;
  }

  acquire(&q->lock);
  // for debug, see below
  int is_full_numproc = (q->numproc >= NPROC);
  int is_full = 0;

  if (q->head == NULL){
    q->head = n;
  } else {
    struct level_node *nn = q->head;
    int i;

    for (i = 0; nn->next != NULL && i < NPROC; nn = nn->next, ++i)
      ;
    if (i >= NPROC || nn->next != NULL) {
      is_full = 1;  // no free slot in level found
    } else {
      nn->next = n;
    }
  }

  if (is_full && is_full_numproc) {
    panic("enqueue in full level");
  } else if (is_full && !is_full_numproc) {
    panic("enqueue in full level, detected late due to faulty accounting of numproc");
  } else {  // enqueue successful
    if (!is_full && is_full_numproc)
      cprintf("enqueue full level FALSE POSITIVE due to faulty accounting of numproc");

    n->next = NULL;
    q->numproc++;   // increment number of procs in this level
  }
  release(&q->lock);
}

// NOTE: *un*queue intentional since proc in middle of queue can be removed
struct level_node*
unqueue_proc(struct proc *p, struct level_queue *q)
{
  if (p == NULL) {
    panic("unqueue of NULL proc");
    return NULL;
  }

  if (q->head == NULL) {
    if (q->numproc != 0)
      panic("unqueue on empty level, mismatch due to faulty accounting of numproc");
    else
      panic("unqueue on empty level");

    return NULL;
  }

  if (q->numproc == 0) {
    cprintf("unqueue empty FALSE POSITIVE due to faulty accounting of numproc");
  }

  struct level_node *n = q->head;
  int found = 0;

  acquire(&q->lock);
  if (&q->head->proc == p) {
    found = 1;
    q->head = n->next->next;
  } else {
    struct level_node *nn = q->head;
    int i;

    for (i = 0; nn->next != NULL && i < NPROC; nn = nn->next, ++i) {
      if (&nn->next->proc == p) {
        // found proc, remove from current queue linked list
        n = nn->next;
        found = 1;
        nn->next = nn->next->next;
        break;
      }
    }
  }

  if (found) {
    n->next = NULL;
    q->numproc--;   // increment number of procs in this level
  }
  release(&q->lock);

  if (!found) {
    panic("unqueue of node not belonging to level");
    return NULL;
  }

  // we only reach here if unqueue is successful
  return n;
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
  struct level_node *n;
  char *sp;

  acquire(&ptable.lock);

  for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
    p = &n->proc;
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

  // only enqueue here since we are sure that allocation is successful
  // Phase 1: always enqueue in our single active level
  struct level_queue *q = &ptable.active[RSDL_STARTING_LEVEL];
  enqueue_node(n, q);

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

    // fork failed, remove forked embryo process from level/queue
    // Phase 1: ALL procs are in first level, so we always remove procs from there
    struct level_queue *q = &ptable.active[RSDL_STARTING_LEVEL];
    unqueue_proc(np, q);
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
  struct level_node *n;
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
  for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
    p = &n->proc;
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

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
  struct level_node *n;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
      p = &n->proc;
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

        // Process exited, remove from its queue
        // Phase 1: ALL procs are in first level, so we always remove procs from there
        struct level_queue *q = &ptable.active[RSDL_STARTING_LEVEL];
        unqueue_proc(p, q);
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
  struct proc *p;
  struct level_node *n;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
      p = &n->proc;
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->ticks_left = RSDL_PROC_QUANTUM;
      if (schedlog_active) {
        if (ticks > schedlog_lasttick) {
          schedlog_active = 0;
        } else {
          struct proc *pp;
          struct level_node *nn;

          cprintf("%d|active|0(0)", ticks);
          for(nn = &ptable.node[0]; nn < &ptable.node[RSDL_LEVELS*NPROC]; nn++){
            pp = &nn->proc;
            if (pp->state == UNUSED) continue;
            else cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->ticks_left);
          }

          cprintf("\n");
        }
      }

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
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
  struct level_node *n;

  for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
    p = &n->proc;
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
  struct level_node *n;

  acquire(&ptable.lock);
  for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
    p = &n->proc;
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
  struct level_node *n;
  char *state;
  uint pc[10];

  for(n = &ptable.node[0]; n < &ptable.node[RSDL_LEVELS*NPROC]; n++){
    p = &n->proc;
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
