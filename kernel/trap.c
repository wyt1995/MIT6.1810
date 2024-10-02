#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

// lab traps: alarm
void save_trapframe(struct proc *);
void restore_trapframe(struct proc *);

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2) {
    // if the alarm interval is set, count the tick of CPU times
    if (p->alarm_interval) {
      p->passed_ticks += 1;
      if (p->passed_ticks == p->alarm_interval && p->allow_entrant) {
        // prevent other calls to the handler before sigreturn
        p->allow_entrant = 0;

        // back up trap frame (user state), and jump to the alarm handler by setting epc
        save_trapframe(p);
        p->trapframe->epc = (uint64) p->alarm_handler;

        // restart the tick counting
        p->passed_ticks = 0;
      }
    }
    yield();
  }

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

int
sigalarm(int ticks, uint64 handler)
{
  struct proc *p = myproc();
  p->alarm_interval = ticks;
  p->alarm_handler = handler;
  return 0;
}

// user alarm handlers are required to make a sigreturn system call
// when they have finished
int
sigreturn()
{
  struct proc *p = myproc();
  restore_trapframe(p);
  p->allow_entrant = 1;
  return p->trapframe->a0;
}

void
save_trapframe(struct proc *p)
{
  p->alarm_frame.epc = p->trapframe->epc;
  p->alarm_frame.ra = p->trapframe->ra;
  p->alarm_frame.sp = p->trapframe->sp;
  p->alarm_frame.gp = p->trapframe->gp;
  p->alarm_frame.tp = p->trapframe->tp;
  p->alarm_frame.t0 = p->trapframe->t0;
  p->alarm_frame.t1 = p->trapframe->t1;
  p->alarm_frame.t2 = p->trapframe->t2;
  p->alarm_frame.s0 = p->trapframe->s0;
  p->alarm_frame.s1 = p->trapframe->s1;
  p->alarm_frame.a0 = p->trapframe->a0;
  p->alarm_frame.a1 = p->trapframe->a1;
  p->alarm_frame.a2 = p->trapframe->a2;
  p->alarm_frame.a3 = p->trapframe->a3;
  p->alarm_frame.a4 = p->trapframe->a4;
  p->alarm_frame.a5 = p->trapframe->a5;
  p->alarm_frame.a6 = p->trapframe->a6;
  p->alarm_frame.a7 = p->trapframe->a7;
  p->alarm_frame.s2 = p->trapframe->s2;
  p->alarm_frame.s3 = p->trapframe->s3;
  p->alarm_frame.s4 = p->trapframe->s4;
  p->alarm_frame.s5 = p->trapframe->s5;
  p->alarm_frame.s6 = p->trapframe->s6;
  p->alarm_frame.s7 = p->trapframe->s7;
  p->alarm_frame.s8 = p->trapframe->s8;
  p->alarm_frame.s9 = p->trapframe->s9;
  p->alarm_frame.s10 = p->trapframe->s10;
  p->alarm_frame.s11 = p->trapframe->s11;
  p->alarm_frame.t3 = p->trapframe->t3;
  p->alarm_frame.t4 = p->trapframe->t4;
  p->alarm_frame.t5 = p->trapframe->t5;
  p->alarm_frame.t6 = p->trapframe->t6;
}

void
restore_trapframe(struct proc *p)
{
  p->trapframe->epc = p->alarm_frame.epc;
  p->trapframe->ra = p->alarm_frame.ra;
  p->trapframe->sp = p->alarm_frame.sp;
  p->trapframe->gp = p->alarm_frame.gp;
  p->trapframe->tp = p->alarm_frame.tp;
  p->trapframe->t0 = p->alarm_frame.t0;
  p->trapframe->t1 = p->alarm_frame.t2;
  p->trapframe->s0 = p->alarm_frame.s0;
  p->trapframe->s1 = p->alarm_frame.s1;
  p->trapframe->a0 = p->alarm_frame.a0;
  p->trapframe->a1 = p->alarm_frame.a1;
  p->trapframe->a2 = p->alarm_frame.a2;
  p->trapframe->a3 = p->alarm_frame.a3;
  p->trapframe->a4 = p->alarm_frame.a4;
  p->trapframe->a5 = p->alarm_frame.a5;
  p->trapframe->a6 = p->alarm_frame.a6;
  p->trapframe->a7 = p->alarm_frame.a7;
  p->trapframe->s2 = p->alarm_frame.s2;
  p->trapframe->s3 = p->alarm_frame.s3;
  p->trapframe->s4 = p->alarm_frame.s4;
  p->trapframe->s5 = p->alarm_frame.s5;
  p->trapframe->s6 = p->alarm_frame.s6;
  p->trapframe->s7 = p->alarm_frame.s7;
  p->trapframe->s8 = p->alarm_frame.s8;
  p->trapframe->s9 = p->alarm_frame.s9;
  p->trapframe->s10 = p->alarm_frame.s10;
  p->trapframe->s11 = p->alarm_frame.s11;
  p->trapframe->t3 = p->alarm_frame.t3;
  p->trapframe->t4 = p->alarm_frame.t4;
  p->trapframe->t5 = p->alarm_frame.t5;
  p->trapframe->t6 = p->alarm_frame.t6;
}
