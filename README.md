# Project 02: xv6 Kernel Thread Implementation

> HYU ELE3021 Operating Systems - 2023071212
> 강수용 교수님의 강의(성준모 조교님)을 수강하며 학습한 내용을 정리하였습니다.

---

## 프로젝트 개요

xv6 RISC-V 운영체제에 커널 레벨 스레드를 구현하는 과제.
싱글 프로세서(CPUS=1) 환경에서 `clone()`/`join()` 시스템콜과
`thread_create()`/`thread_join()` 유저 라이브러리를 구현한다.

---

## 수정된 파일 목록

| 파일 | 역할 |
|------|------|
| `kernel/proc.h` | `struct proc`에 스레드 필드 추가 |
| `kernel/proc.c` | `clone()`, `join()`, `freeproc()`, `growproc()`, `exit()`, `kill()` 등 수정 |
| `kernel/exec.c` | 스레드에서 exec 호출 시 페이지 테이블 정리 |
| `kernel/trap.c` | `usertrapret()`에서 스레드별 `trapframe_va` 사용 |
| `kernel/syscall.h` | `SYS_clone=22`, `SYS_join=23` 정의 |
| `kernel/syscall.c` | 시스콜 디스패처에 clone/join 등록 |
| `kernel/sysproc.c` | `sys_clone()`, `sys_join()` 래퍼 |
| `kernel/defs.h` | clone/join 커널 함수 선언 |
| `user/user.h` | clone/join 유저 선언 |
| `user/usys.pl` | clone/join 스텁 생성 |
| `user/thread.h` | `thread_create`/`thread_join` API |
| `user/thread.c` | 유저 스레드 라이브러리 구현 |
| `user/umalloc.c` | 스레드 안전 malloc/free (amoswap 기반 락) |
| `Makefile` | `thread.o`를 ULIB에 추가, 테스트 프로그램 빌드 |

---

## 핵심 설계

### 스레드 = struct proc (페이지 테이블 공유)

```
fork():  부모 페이지 테이블을 복사 (독립 주소공간)
clone(): 부모 페이지 테이블을 공유 (같은 주소공간)
```

`clone()`으로 생성된 스레드는 `np->pagetable = p->pagetable`로 부모와
동일한 페이지 테이블을 사용한다. 힙, 전역변수, 코드 영역을 공유하되
각 스레드는 독립적인 커널 스택, trapframe, 레지스터 컨텍스트를 갖는다.

### 스레드별 trapframe 가상 주소

```
메인 프로세스: TRAPFRAME                        (0x3FFFFFFE000)
스레드 1:      TRAPFRAME - (pid1 * PGSIZE)
스레드 2:      TRAPFRAME - (pid2 * PGSIZE)
...
```

각 스레드는 고유한 trapframe 물리 페이지를 `kalloc()`으로 할당받고,
공유 페이지 테이블의 서로 다른 가상 주소에 매핑한다.
`usertrapret()`에서 `sscratch`를 해당 스레드의 `trapframe_va`로 설정하여
트랩 진입 시 올바른 trapframe에 레지스터를 저장/복원한다.

### struct proc 추가 필드

```c
struct proc {
  // ... 기존 필드 ...
  struct proc *main_thread;  // 메인 스레드 포인터 (fork: self, clone: parent)
  int isThread;              // 1이면 스레드, 0이면 프로세스
  uint64 thread_stack;       // clone 시 전달받은 유저 스택 원본 주소
};
```

---

## 발견된 버그와 수정 내용

### 버그 1: join()이 잘못된 스택 주소를 반환 (치명적)

**파일**: `kernel/proc.c` - `join()`

**증상**: TEST#4(스레드 malloc/free)에서 `usertrap(): scause 0xf stval=0x8` 발생.
malloc의 free list 순회 중 NULL 포인터 역참조로 store page fault.

**원인 분석**:

```c
// 기존 코드 (버그)
uint64 stack_addr = pp->trapframe->sp - PGSIZE;
```

`trapframe->sp`는 스레드가 `exit()` 호출 시점의 스택 포인터이다.
스레드 실행 중 스택이 사용되므로 sp는 초기값보다 작아진다.
따라서 `sp - PGSIZE`는 malloc이 반환한 원래 주소가 아닌 엉뚱한 주소가 된다.

```
thread_create: stack = malloc(8192) → 예: 0x5000
clone:         sp = stack + PGSIZE = 0x6000
스레드 실행 후: sp = 0x5F00 (스택 사용)
exit 시:       trapframe->sp = 0x5F00
join 계산:     0x5F00 - 0x1000 = 0x4F00  ← malloc한 적 없는 주소!
thread_join:   free(0x4F00)  ← 힙 free list 파괴!
```

TEST#1~3에서 매번 `thread_join()`이 호출될 때마다 `free(엉뚱한 주소)`가
실행되어 malloc의 순환 연결 리스트(free list)가 점진적으로 파괴된다.
TEST#4가 시작될 때 free list는 이미 손상되어 있고, 스레드들이 `malloc()`을
호출하면 손상된 리스트를 순회하다가 NULL 포인터에 도달 → page fault.

**수정**: `thread_stack` 필드를 추가하여 clone 시 원래 스택 주소를 저장.

```c
// proc.h
uint64 thread_stack;  // 추가

// clone() - 원래 스택 주소 저장
np->thread_stack = (uint64)stack;

// join() - 저장된 원래 주소 사용
uint64 stack_addr = pp->thread_stack;  // trapframe->sp - PGSIZE 대신
```

**검증 방법**: TEST#4에서 5개 스레드가 각각 1000번 malloc/free를 반복할 때
usertrap 없이 완료되는지 확인.

---

### 버그 2: freeproc() 페이지 테이블 해제 로직 반전 (치명적)

**파일**: `kernel/proc.c` - `freeproc()`

**증상**: 공유 페이지 테이블이 다른 스레드가 사용 중인데도 해제됨 (use-after-free).
혹은 아무도 사용하지 않는데 해제되지 않음 (메모리 누수).

**원인 분석**:

```c
// 기존 코드 (버그) - 같은 그룹이면 skip, 다른 그룹이면 target=0
for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp == p) continue;
    if(pp->state == UNUSED) continue;
    if(pp->main_thread == p->main_thread) continue;  // 같은 그룹 skip!
    target = 0;  // 다른 그룹이 있으면 해제 안 함
    break;
}
```

이 로직은 의도와 정반대로 동작한다:
- 같은 스레드 그룹(페이지 테이블 공유)을 skip하므로, 형제 스레드가 살아있어도 target=1 유지
- 결과: 형제 스레드가 아직 실행 중인데 페이지 테이블이 해제됨!

예시: 메인 P, 스레드 T1, T2. T1이 종료되어 freeproc 실행:
```
pp=P:  main_thread=P == T1의 main_thread=P → continue (skip!)
pp=T2: main_thread=P == T1의 main_thread=P → continue (skip!)
→ target=1 → 페이지 테이블 해제! P와 T2는 해제된 페이지 테이블로 실행됨!
```

**수정**: 같은 페이지 테이블을 공유하는 프로세스가 있으면 해제하지 않도록 변경.

```c
// 수정된 코드 - 같은 페이지 테이블을 공유하면 해제하지 않음
for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp == p) continue;
    if(pp->state == UNUSED) continue;
    if(pp->pagetable == p->pagetable){  // 공유 중이면
      target = 0;                        // 해제하지 않음
      break;
    }
}
```

**검증 방법**: 여러 스레드 생성/종료를 반복해도 다른 스레드가 정상 실행되는지 확인.

---

### 버그 3: clone()에서 allocproc()이 만든 페이지 테이블 누수

**파일**: `kernel/proc.c` - `clone()`

**증상**: 스레드 생성마다 페이지 테이블 3페이지(L2+L1+L0)가 누수되어
장기 실행 시 커널 메모리 고갈.

**원인 분석**:

```c
// allocproc()이 새 페이지 테이블 생성 (TRAMPOLINE + TRAPFRAME 매핑 포함)
np = allocproc();

// clone()이 부모 페이지 테이블로 덮어쓰기 → 이전 페이지 테이블 누수!
np->pagetable = p->pagetable;
```

`allocproc()`이 `proc_pagetable()`으로 새 페이지 테이블을 만들지만,
`clone()`은 부모의 페이지 테이블을 공유하므로 즉시 덮어쓴다.
이때 이전 페이지 테이블을 해제하지 않으면 메모리 누수가 발생한다.

**수정**: 덮어쓰기 전에 임시 페이지 테이블을 해제.

```c
// allocproc이 만든 임시 페이지 테이블 해제 (물리 페이지는 해제하지 않음)
proc_freepagetable(np->pagetable, 0);
np->pagetable = p->pagetable;  // 부모 페이지 테이블 공유
```

---

### 버그 4: growproc() 실패 시 wait_lock 미해제 (데드락)

**파일**: `kernel/proc.c` - `growproc()`

**증상**: `uvmalloc()` 실패 시 `wait_lock`을 해제하지 않고 리턴.
이후 어떤 프로세스도 `wait_lock`을 acquire하지 못해 시스템 전체 데드락.

**원인 분석**:

```c
// 기존 코드 (버그)
acquire(&wait_lock);
sz = p->sz;
if(n > 0){
    if((sz = uvmalloc(...)) == 0) {
        return -1;  // wait_lock 해제 안 함!
    }
}
```

**수정**:

```c
if((sz = uvmalloc(...)) == 0) {
    release(&wait_lock);  // 추가
    return -1;
}
```

---

### 버그 5: umalloc.c의 malloc/free가 스레드 안전하지 않음

**파일**: `user/umalloc.c`

**증상**: TEST#4에서 여러 스레드가 동시에 malloc/free 호출 시 free list 손상.
타이머 인터럽트에 의한 컨텍스트 스위치가 malloc 도중에 발생하면
다른 스레드가 같은 free list를 동시에 조작하여 구조가 파괴됨.

**원인 분석**: xv6의 기본 umalloc.c는 싱글 프로세스용으로 설계되어
전역 `freep` 포인터에 대한 동기화가 전혀 없다.

**수정**: RISC-V `amoswap` 원자 명령어 기반의 유저 레벨 스핀락 추가.

```c
static volatile int malloc_lock = 0;

static void acquire_malloc(void) {
    int old;
    for(;;){
        asm volatile("amoswap.w.aq %0, %1, (%2)"
            : "=r"(old) : "r"(1), "r"(&malloc_lock) : "memory");
        if(old == 0) break;
        sleep(1);  // 싱글 CPU: yield하여 락 소유자가 실행되도록
    }
    asm volatile("fence rw, rw" ::: "memory");
}

static void release_malloc(void) {
    asm volatile("fence rw, rw" ::: "memory");
    asm volatile("amoswap.w.rl zero, %0, (%1)"
        : : "r"(0), "r"(&malloc_lock) : "memory");
}
```

주의사항:
- `morecore()`가 `malloc()` 내부에서 호출되므로 내부용 `_free()`를 분리하여
  재진입 데드락을 방지
- 싱글 CPU에서는 락 소유자가 선점되면 다른 스레드가 영원히 스핀하므로
  `sleep(1)`로 CPU를 양보해야 함

---

### 버그 6: exec()에서 스레드의 trapframe 매핑 미정리 (panic: freewalk: leaf)

**파일**: `kernel/exec.c`

**증상**: TEST#6에서 스레드가 `exec()` 호출 후 `panic: freewalk: leaf` 발생.

**원인 분석**: 스레드가 exec()를 호출하면:

1. 새 페이지 테이블을 만들고 프로그램을 로드
2. `p->pagetable = 새 페이지 테이블`로 교체

이때 옛 공유 페이지 테이블에 남아있는 이 스레드의 trapframe 매핑
(`TRAPFRAME - pid*PGSIZE`)이 정리되지 않는다.

나중에 ZOMBIE 스레드들이 모두 정리되고 마지막으로 `proc_freepagetable()`이
호출되면, `uvmfree()` → `freewalk()`가 이 남은 leaf PTE를 발견하고 panic.

추가 함정: `proc_pagetable(p)`에 `p->trapframe_va = TRAPFRAME`이라는
**부작용**이 있어, exec 내에서 `proc_pagetable()` 호출 후에는
`p->trapframe_va`가 이미 `TRAPFRAME`으로 덮어씌워진 상태다.

```c
// proc_pagetable() 내부 (line 221)
mappages(pagetable, p->trapframe_va = TRAPFRAME, ...);
//                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//                  부작용: p->trapframe_va를 TRAPFRAME으로 변경!
```

따라서 exec의 commit 단계에서 `p->trapframe_va`를 사용하면
스레드의 원래 값(`TRAPFRAME - pid*PGSIZE`)이 아닌 `TRAPFRAME`을 unmap하게 됨.

**수정**: exec() 시작 시 원래 `trapframe_va`를 미리 저장.

```c
// exec() 시작부
uint64 old_trapframe_va = p->trapframe_va;  // proc_pagetable 호출 전에 저장

// ... proc_pagetable(p) 호출 (여기서 p->trapframe_va가 TRAPFRAME으로 변경됨) ...

// commit 직전: 옛 페이지 테이블에서 스레드의 trapframe 매핑 제거
pagetable_t oldpagetable = p->pagetable;
uint64 oldsz = p->sz;
if(p->isThread){
    pte_t *pte = walk(oldpagetable, old_trapframe_va, 0);  // 저장해둔 원래 값 사용!
    if(pte && (*pte & PTE_V))
        uvmunmap(oldpagetable, old_trapframe_va, 1, 0);
}

// commit
p->pagetable = pagetable;
p->isThread = 0;
p->thread_stack = 0;

// 옛 페이지 테이블이 공유 중이 아니면 해제
int shared = 0;
for(struct proc *pp = proc; pp < &proc[NPROC]; pp++){
    if(pp == p) continue;
    if(pp->state == UNUSED) continue;
    if(pp->pagetable == oldpagetable){ shared = 1; break; }
}
if(!shared) proc_freepagetable(oldpagetable, oldsz);
```

**검증 방법**: TEST#6에서 스레드가 exec 호출 후 panic 없이 프로그램이 실행되고,
이후 ZOMBIE 정리 과정에서도 정상 동작하는지 확인.

---

### 버그 7 (기존 코드에 이미 존재): growproc() sz 동기화 조건 오류

**파일**: `kernel/proc.c` - `growproc()`

**증상**: `sbrk()` 호출 후 같은 스레드 그룹의 다른 스레드에 sz가 반영되지 않아
다른 스레드의 `sbrk(0)` 호출 시 오래된 sz를 반환.

**원인**: 기존 코드는 `pp->main_thread == p` 조건으로 sz를 동기화했는데,
이는 스레드가 sbrk를 호출하는 경우를 처리하지 못함.

```c
// 기존 (버그)
if(pp->state != UNUSED && pp->pagetable == p->pagetable && pp->main_thread == p)

// 수정
if(pp->state != UNUSED && pp->pagetable == p->pagetable)
```

---

## 디버깅 과정 요약

### 크래시 추적 과정 (TEST#4 usertrap)

1. `usertrap(): scause 0xf stval=0x8` 분석
   - scause 0xf = Store/AMO page fault (RISC-V)
   - stval=0x8 = 주소 0x8에 쓰기 시도 → NULL+8 역참조

2. thread_test.asm에서 sepc 매핑
   ```
   f3c: 4137073b  subw a4,a4,s3    # p->s.size -= nunits
   f40: c518      sw a4, 8(a0)     # ← 크래시: a0=0 (p가 NULL)
   ```
   malloc의 블록 분할 코드에서 free list 포인터 p가 NULL

3. 원인 역추적: free list가 왜 NULL을 포함하는가?
   - K&R malloc의 free list는 순환 연결 리스트 → NULL이 있으면 안 됨
   - `free(잘못된 주소)` 호출이 리스트 구조를 파괴
   - `join()`이 반환하는 스택 주소가 잘못됨 → `thread_join()`의 `free(stack)` 호출이 원인

### 크래시 추적 과정 (TEST#6 freewalk: leaf)

1. `panic: freewalk: leaf` = 페이지 테이블 해제 시 아직 매핑된 leaf PTE 발견
2. exec()에서 새 페이지 테이블로 교체 후, 옛 페이지 테이블에 스레드의 trapframe 매핑이 잔존
3. `proc_pagetable()`의 부작용(`p->trapframe_va = TRAPFRAME`)으로 인해
   exec에서 원래 trapframe_va 값이 소실됨
4. 해결: exec() 진입 직후 `old_trapframe_va`를 별도 변수에 저장

---

## 테스트 실행 방법

```bash
# 빌드 및 QEMU 실행
make qemu

# xv6 셸에서 테스트 실행
$ thread_test

# QEMU 종료: Ctrl+A → X
```

### 테스트 항목

| 테스트 | 내용 |
|--------|------|
| TEST#1 | 기본 스레드 생성/종료/join |
| TEST#2 | 스레드 독립 실행 (카운터 증가) |
| TEST#3 | 스레드 내에서 fork() 호출 |
| TEST#4 | 스레드 sbrk/malloc 동시 사용 (5스레드 x 1000회) |
| TEST#5 | kill()로 스레드 그룹 종료 |
| TEST#6 | 스레드 내에서 exec() 호출 |

---

## 시스템콜 흐름

### thread_create → clone

```
user: thread_create(fcn, arg1, arg2)
  → malloc(8192)로 스택 할당
  → clone(fcn, arg1, arg2, stack) 시스콜
      → allocproc()으로 새 proc 할당
      → 임시 페이지 테이블 해제 (allocproc이 만든 것)
      → np->pagetable = p->pagetable (부모와 공유)
      → np->thread_stack = stack (원본 스택 주소 저장)
      → trapframe에 epc=fcn, a0=arg1, a1=arg2, sp=stack+PGSIZE 설정
      → trapframe_va = TRAPFRAME - (pid * PGSIZE)로 공유 페이지 테이블에 매핑
      → RUNNABLE로 전환
```

### thread_join → join

```
user: thread_join()
  → join(&stack) 시스콜
      → 프로세스 테이블에서 main_thread==p && isThread인 ZOMBIE 탐색
      → pp->thread_stack을 유저 공간에 복사 (올바른 원본 스택 주소)
      → freeproc(pp): trapframe_va unmap, 공유 여부 확인 후 페이지 테이블 처리
  → free(stack)으로 스택 메모리 반환
```
