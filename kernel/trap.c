#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"

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

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if (r_scause() == 13 || r_scause() == 15) {
    // 造成中断的虚拟地址
    uint64 va = PGROUNDDOWN(r_stval());
    struct vma *vma = 0;
    for (int i = 0; i < NVMA; i++) {
      // 遍历查找是否有能包含 va 的 vma
      if (p->vmas[i].valid && 
          va >= p->vmas[i].addr && 
          va < p->vmas[i].addr + p->vmas[i].len) {
            // 若 vma 有效且在范围内
            vma = &p->vmas[i];
            break;
          }
    }

    // 若找不到合适的 vma，说明就是发生了缺页，而不是由懒加载造成的
    if (vma == 0) goto err;

    // 当且仅当 store 指令，且为 PROT_WRITE，且已经分配内存、页表时，设置页表项为脏
    // 也就是说，当内存、页表未分配，且第一条访问该地址的指令为 store 时，会产生两次中断
    if (r_scause() == 15 && (vma->prot & PROT_WRITE) &&
        walkaddr(p->pagetable, va)) {
      if (uvmdirtywriteset(p->pagetable, va)) {
        printf("uvmdirtywriteset err\n");
        goto err;
      }

    } else {
      // 分配一个物理页
      uint64 pa = (uint64) kalloc();
      if (pa == 0) goto err;
      memset((void *)pa, 0, PGSIZE);

      // 下一步实质是将磁盘或bcache上的文件数据，
      // 通过vma记录的inode，读出文件的一页到内存中
      int now_offset = va - vma->addr;
      ilock(vma->file->ip);
      // 文件大部分从头读，vma->offset 一般为 0

      // 为什么这里 readi 的 user_dst 参数为 0 ？
      // 因为我们尚未建立 va -> pa 的映射关系，
      // 所以我们直接使用内核页表 一一对应 的映射关系索引到物理内存
      // 我们使用内核页表，因此 user_dst 参数为 0
      if (readi(vma->file->ip, 0, pa, vma->offset + now_offset, PGSIZE) < 0) {
        iunlock(vma->file->ip);
        printf("usertrap: readi err\n");
        goto err;
      }
      iunlock(vma->file->ip);

      // 设置页表及其权限
      int perm = PTE_U;
      if (vma->prot & PROT_READ)
        perm |= PTE_R;
      if (vma->prot & PROT_EXEC)
        perm |= PTE_X;
      if (mappages(p->pagetable, va, PGSIZE, pa, perm) < 0) {
        kfree((void*)pa);
        goto err;
      }
    }

  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
err:
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

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

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
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

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
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
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
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

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
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
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

