#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

// Memory allocator by Kernighan and Ritchie,
// The C Programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;

// Thread-safe lock using RISC-V atomic swap (amoswap)
static volatile int malloc_lock = 0;

static void
acquire_malloc(void)
{
  int old;
  for(;;){
    asm volatile(
      "amoswap.w.aq %0, %1, (%2)"
      : "=r"(old)
      : "r"(1), "r"(&malloc_lock)
      : "memory"
    );
    if(old == 0)
      break;
    // Single CPU: must yield so lock holder can run and release
    sleep(1);
  }
  asm volatile("fence rw, rw" ::: "memory");
}

static void
release_malloc(void)
{
  asm volatile("fence rw, rw" ::: "memory");
  asm volatile(
    "amoswap.w.rl zero, %0, (%1)"
    :
    : "r"(0), "r"(&malloc_lock)
    : "memory"
  );
}

// Internal free (no lock) - called from morecore inside malloc
static void
_free(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

void
free(void *ap)
{
  acquire_malloc();
  _free(ap);
  release_malloc();
}

static Header*
morecore(uint nu)
{
  char *p;
  Header *hp;

  if(nu < 4096)
    nu = 4096;
  p = sbrk(nu * sizeof(Header));
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  _free((void*)(hp + 1));
  return freep;
}

void*
malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;

  acquire_malloc();

  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      release_malloc();
      return (void*)(p + 1);
    }
    if(p == freep)
      if((p = morecore(nunits)) == 0){
        release_malloc();
        return 0;
      }
  }
}
