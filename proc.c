#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#define AGE_CUTOFF 200

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

#ifdef MLFQ
  struct procQueue proc_queue[5];
#endif

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");

  #ifdef MLFQ
    int i;
    for(i=0; i<5; i++){
      proc_queue[i].timeslice_cutoff = (1 << i);
      proc_queue[i].largest_position = 0;
    }
  #endif
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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  //cprintf("->%d\n", ticks);

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

  // initializing custom values
  p->ctime = ticks;
  p->etime = p->ctime;
  p->rtime = 0;
  p->tmp_wtime = 0;
  p->priority = 60;
  p->n_run = 0;

  p->timeslice = 0;
  p->position_priority = 0;

  // flags
  p->io = 0;
  p->tickflag = -1;

  #ifdef MLFQ
    p->cur_q = 0;
    #ifdef BONUS
      // For bonus part
      cprintf("%d,%d,%d,Init\n", p->pid, p->cur_q, ticks);
      // cprintf("-> [%d] %d %d (Init)\n", p->pid, p->cur_q, ticks);
    #endif
  #else
    p->cur_q = -1;
  #endif

  for(int i=0; i<5; i++){
    p->q[i] = 0;
  }

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

  curproc->etime = ticks;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  
  #ifdef MLFQ
    #ifdef BONUS
      // for bonus part
      cprintf("%d,%d,%d,Exit\n", curproc->pid, curproc->cur_q, ticks);
      // cprintf("-> [%d] %d %d (Exit)\n", curproc->pid, curproc->cur_q, ticks);
    #endif
  #endif

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
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
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

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// gives waiting time and running time of the process
int
waitx(int* wtime, int* rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        // cprintf("->%d %d %d\n", p->ctime, p->etime, p->rtime);
        *rtime = p->rtime;
        *wtime = p->etime - p->ctime - p->rtime + 1; // total wait time // adding 1 as when process is initiated, picked by scheduler and ends in the same tick, then run time will be 1.
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
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  #ifdef RR
    cprintf("---> DEFAULT\n");
    for(;;){
      // Enable interrupts on this processor.
      sti();

      // Loop over process table looking for process to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        
        // custom updates
        p->n_run++;
        p->tmp_wtime = 0;
        p->io = 0;

        // for handling runtime in 1 tick when process is picked up by scheduler
        if(p->tickflag != ticks){
          p->tickflag = ticks;
          p->rtime++;
        }

        swtch(&(c->scheduler), p->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
  #endif
  #ifdef FCFS
    cprintf("---> FCFS\n");
    for(;;){
      struct proc *earliestProcess = 0;
      // Enable interrupts on this processor.
      sti();

      // Loop over process table looking for process with lowest ctime to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        if(earliestProcess == 0){
          earliestProcess = p;
        }
        else if(p->ctime < earliestProcess->ctime){
          earliestProcess = p;
        }
        // cprintf("%d\n", earliestProcess->pid);
      }
      if(earliestProcess != 0){
        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = earliestProcess;
        switchuvm(earliestProcess);
        earliestProcess->state = RUNNING;
        
        // custom updates
        earliestProcess->n_run++;
        earliestProcess->tmp_wtime = 0;
        earliestProcess->io = 0;
        // cprintf("Pid: %d running.\n", earliestProcess->pid);

        // for handling runtime in 1 tick when process is picked up by scheduler
        if(earliestProcess->tickflag != ticks){
          earliestProcess->tickflag = ticks;
          earliestProcess->rtime++;
        }

        swtch(&(c->scheduler), earliestProcess->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
  #endif
  #ifdef PBS
    cprintf("---> PBS\n");
    for(;;){
      struct proc *highestPriorityProcess = 0;
      // Enable interrupts on this processor.
      sti();

      // Loop over process table looking for process with highest priority to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        if(highestPriorityProcess == 0){
          highestPriorityProcess = p;
        }
        else if(p->priority < highestPriorityProcess->priority){
          highestPriorityProcess = p;
        }
        // cprintf("%d\n", highestPriorityProcess->pid);
      }

      if(highestPriorityProcess != 0){
        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = highestPriorityProcess;
        switchuvm(highestPriorityProcess);
        highestPriorityProcess->state = RUNNING;
        
        // custom updates
        highestPriorityProcess->n_run++;
        highestPriorityProcess->tmp_wtime = 0;
        highestPriorityProcess->io = 0;

        // for handling runtime in 1 tick when process is picked up by scheduler
        if(highestPriorityProcess->tickflag != ticks){
          highestPriorityProcess->tickflag = ticks;
          highestPriorityProcess->rtime++;
        }

        swtch(&(c->scheduler), highestPriorityProcess->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
  #endif
  #ifdef MLFQ
    cprintf("---> MLFQ\n");
    for(;;){
      struct proc *highestPriorityProcess = 0;
      // Enable interrupts on this processor.
      sti();

      // Loop over process table looking for process with highest priority to run.
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        if(highestPriorityProcess == 0){
          highestPriorityProcess = p;
        }
        else if(p->cur_q < highestPriorityProcess->cur_q){
          highestPriorityProcess = p;
        }
        else if(p->cur_q == highestPriorityProcess->cur_q){
          if(p->position_priority < highestPriorityProcess->position_priority){
            highestPriorityProcess = p;
          }
        }
        // cprintf("%d\n", highestPriorityProcess->pid);
      }

      if(highestPriorityProcess != 0){
        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = highestPriorityProcess;
        switchuvm(highestPriorityProcess);
        highestPriorityProcess->state = RUNNING;
        
        // custom updates
        highestPriorityProcess->n_run++;
        highestPriorityProcess->tmp_wtime = 0;
        highestPriorityProcess->io = 0;

        // for handling runtime in 1 tick when process is picked up by scheduler
        if(highestPriorityProcess->tickflag != ticks){
          highestPriorityProcess->tickflag = ticks;
          highestPriorityProcess->rtime++;
          // number of ticks a process received in its queue
          highestPriorityProcess->q[highestPriorityProcess->cur_q]++;
        }

        swtch(&(c->scheduler), highestPriorityProcess->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
  #endif
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

  #ifdef MLFQ
    // Process goes to sleep if it waits for io
    // Hence it is pushed at the end of queue i.e. the maximum value of position_priority is allocated 
    if(p->io == 0){
      p->io = 1;
      p->position_priority = 1 + proc_queue[p->cur_q].largest_position;
      proc_queue[p->cur_q].largest_position = p->position_priority;
      // for bonus part
      // cprintf("%d,%d,%d,IO\n", p->pid, p->cur_q, ticks);
      // cprintf("-> [%d] %d %d (IO)\n", p->pid, p->cur_q, ticks);
    }
  #endif

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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
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
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
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

// To update running time of the process per clock tick
void updateruntime(void){
  acquire(&ptable.lock);
  struct proc * p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNING){
      p->rtime++;
      p->tmp_wtime = 0;
      p->tickflag = ticks;

      #ifdef MLFQ
        // number of ticks a process received in its queue
        p->q[p->cur_q]++;
      #endif
    }
    else if(p->state != UNUSED){
      p->tmp_wtime++;
    }
     
    #ifdef MLFQ
      if(p->state != UNUSED){
        // for aging
        if(p->tmp_wtime > AGE_CUTOFF){
          // priority is increased
          if(p->cur_q != 0){
            // change in queue
            p->cur_q--;
            // to push to end
            p->position_priority = 1+proc_queue[p->cur_q].largest_position;
            proc_queue[p->cur_q].largest_position = p->position_priority;
            p->tmp_wtime = 0;

            #ifdef BONUS
              // for bonus part
              cprintf("%d,%d,%d,Aging\n", p->pid, p->cur_q, ticks);
              // cprintf("-> [%d] %d %d (Aging)\n", p->pid, p->cur_q, ticks);
            #endif
          }

        }
      }
    #endif
  }
  release(&ptable.lock);
}

// To print details regarding each process
void procdetails(void){
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  acquire(&ptable.lock);
  cprintf("PID\tPriority\tState\tr_time\tw_time\tn_run\tcur_q\tq0\tq1\tq2\tq3\tq4\n");
  struct proc * p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != UNUSED){
      cprintf("%d\t", p->pid);
      cprintf("%d\t\t", p->priority);
      cprintf("%s\t", states[p->state]);
      cprintf("%d\t", p->rtime);
      cprintf("%d\t", p->tmp_wtime);
      cprintf("%d\t", p->n_run);
      cprintf("%d\t", p->cur_q);
      for(int i=0; i<5; i++){
        cprintf("%d\t", p->q[i]);
      }
      cprintf("\n");
    }
  }
  release(&ptable.lock);
}

// To set priority of a process
int 
set_priority(int new_priority, int pid){
  acquire(&ptable.lock);
  struct proc * p;
  int oldPriority = -1;

  // when priority is not in the range [0,100]
  if(new_priority < 0){
    new_priority = 0;
  }
  else if(new_priority > 100){
    new_priority = 100;
  }

  // cprintf("%d %d\n", new_priority, pid);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != UNUSED){
      if(p->pid == pid){
        oldPriority = p->priority;
        p->priority = new_priority;
        break;
      }
    }
  }
  release(&ptable.lock);
  return oldPriority;
}