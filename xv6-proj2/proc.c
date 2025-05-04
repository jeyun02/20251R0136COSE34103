#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// Ready queue head
static struct proc *readyqueue_head = 0;

// Enqueue: Insert process into ready queue based on priority and PID
void enqueue(struct proc *p) {
  struct proc **current = &readyqueue_head;

  while (*current) {
    if ((*current)->priority > p->priority) // p 의 priority 값이 작다! 바로 삽입.
      break;
    else if ((*current)->priority == p->priority && (*current)->pid < p->pid) // tiebreak: priority 값은 같은데 pid 값이 크다! 바로 삽입.
      break;
    else
      current = &(*current)->next;
  }

  // enqueue(p) 핵심 삽이ㅣㅂ 
  p->next = *current;
  *current = p;
}

// Dequeue: Remove and return the process at the front of the ready queue
struct proc* dequeue(void) {

  if (!readyqueue_head) { // 비었음.
    return 0;
  }
  struct proc *first = readyqueue_head; 
  readyqueue_head = first->next; // head 를 차기 proc 에게 넘겨줘야 함.
  first->next = 0; // return 전 next 를 끊어놔야 함.

  return first;
}

// Remove a specific process from the ready queue
void remove_from_readyqueue(struct proc *p) {

  if (p == 0) // p 가 비었음
    return;

  struct proc **current = &readyqueue_head;

  while (*current) {
    if (*current == p) {  // 일치하는 proc 찾으면, current 전후를 연결해서 readyqueue 갱신.
      *current = p->next; // 
      p->next = 0;
      return;
    }
    current = &(*current)->next;
  }
}


struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
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

  p->priority = 5;

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  //! RUNNABLE 로 바뀌는 즉시 ready queue 에 삽입
  enqueue(p); 
  //! ---------------------------------------  
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
  // cprintf("ENTER fork\n");

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

  // ! priority 상속 규칙 적용(R1)
  if(curproc->priority >= 15) 
    np->priority = curproc->priority / 2;
  else  
    np->priority = curproc->priority + 1;
  //! ------------------------------

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
  // !RUNNABLE 되면 바로 readyqueue에 넣기
  enqueue(np); 
  // ! ---------------------------

  struct proc *top = readyqueue_head;
  // priority 값이 크면 양보, priority는 같은데 PID 가 작으면 양보
  if (top && (top->priority < curproc->priority || (top->priority == curproc->priority && top->pid > curproc->pid))) {
    release(&ptable.lock);
    yield();
    acquire(&ptable.lock);
  }
  // // ! ---------------------------------
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  // cprintf("ENTER exit\n");

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
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  //! 좀비되면 제거.
  remove_from_readyqueue(curproc);
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  // cprintf("ENTER wait\n");

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
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    // ready queue에서 우선순위가 가장 높은 프로세스 선택
    struct proc *p = dequeue();
    // cprintf("[scheduler] pid: %d 의 우선순위가 제일 높다.\n", p->pid);

    if(p){
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      // 컨텍스트 스위칭 (스케줄러 → 프로세스)
      swtch(&(c->scheduler), p->context);

      // 프로세스 실행 종료 후 커널로 복귀
      switchkvm();

      // 프로세스가 yield/sleep/exit 등으로 돌아오면 여기로 복귀
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
  // cprintf("ENTER yield\n");
  
  acquire(&ptable.lock);  //DOC: yieldlock
  // original code: 
  // myproc()->state = RUNNABLE; 
  // -------------------------
  // changed version! 
  struct proc *curproc = myproc(); //! ready queue 에 넣기 위해 객체 선언
  curproc->state = RUNNABLE; 
  //! RUNNABLE 로 바뀌는 즉시 ready queue 에 삽입
  enqueue(curproc); 
  //! ---------------------------------------  // ------------------
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
  // cprintf("\n[sleep] pid: %d , prio : %d\n", p->pid, p->priority);

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
  // !RUNNABLE 에서 벗어나면 readyqueue 에서 제거.
  // if (p->state == RUNNABLE) remove_from_readyqueue(p); 
  //! 얘가 문젠거 같은데.
  p->chan = chan;
  p->state = SLEEPING;

  sched();
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
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      //! RUNNABLE 로 바뀌는 즉시 ready queue 에 삽입
      enqueue(p); 
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
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
        //! RUNNABLE 로 바뀌는 즉시 ready queue 에 삽입
        enqueue(p);
        //! ---------------------------
      }
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
  [RUNNABLE]  "runble", // 오타난듯.
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

int
setnice(int pid, int nice)
{
  if (nice < 0 || nice > 30)
    return -1;

  struct proc *p;
  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      // RUNNABLE 상태면 readyqueue에서 일단 제거
      if (p->state == RUNNABLE)
        remove_from_readyqueue(p);

      // setnice 핵심
      p->priority = nice;

      // RUNNABLE 이면 다시 readyqueue에 삽입
      if (p->state == RUNNABLE)
        enqueue(p);
      // 현재 실행 중인 프로세스가 자기 자신이고 
      // readyqueue의 top이 더 높은 priority라면 yield함. 선점식으로
      if (p == myproc() && p->state == RUNNING) {
        struct proc *top = readyqueue_head;
        // priority 값이 크면 양보, priority는 같은데 PID 가 작으면 양보
        if (top && (top->priority < p->priority || (top->priority == p->priority && top->pid < p->pid))) {
          release(&ptable.lock);  // yield 전에   release! 해야 panic 안 걸림
          yield();
          return 0;
        }
      }

      release(&ptable.lock);
      return 0;
    }
  }

  release(&ptable.lock);
  return -1;
}
