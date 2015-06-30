/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include "uv.h"
#include "internal.h"


#define HAVE_SRWLOCK_API() (pTryAcquireSRWLockShared != NULL)
#define HAVE_CONDVAR_API() (pInitializeConditionVariable != NULL)

static int uv__rwlock_srwlock_init(uv_rwlock_t* rwlock);
static void uv__rwlock_srwlock_destroy(uv_rwlock_t* rwlock);
static void uv__rwlock_srwlock_rdlock(uv_rwlock_t* rwlock);
static int uv__rwlock_srwlock_tryrdlock(uv_rwlock_t* rwlock);
static void uv__rwlock_srwlock_rdunlock(uv_rwlock_t* rwlock);
static void uv__rwlock_srwlock_wrlock(uv_rwlock_t* rwlock);
static int uv__rwlock_srwlock_trywrlock(uv_rwlock_t* rwlock);
static void uv__rwlock_srwlock_wrunlock(uv_rwlock_t* rwlock);

static int uv__rwlock_fallback_init(uv_rwlock_t* rwlock);
static void uv__rwlock_fallback_destroy(uv_rwlock_t* rwlock);
static void uv__rwlock_fallback_rdlock(uv_rwlock_t* rwlock);
static int uv__rwlock_fallback_tryrdlock(uv_rwlock_t* rwlock);
static void uv__rwlock_fallback_rdunlock(uv_rwlock_t* rwlock);
static void uv__rwlock_fallback_wrlock(uv_rwlock_t* rwlock);
static int uv__rwlock_fallback_trywrlock(uv_rwlock_t* rwlock);
static void uv__rwlock_fallback_wrunlock(uv_rwlock_t* rwlock);


static int uv_cond_fallback_init(uv_cond_t* cond);
static void uv_cond_fallback_destroy(uv_cond_t* cond);
static void uv_cond_fallback_signal(uv_cond_t* cond);
static void uv_cond_fallback_broadcast(uv_cond_t* cond);
static void uv_cond_fallback_wait(uv_cond_t* cond, uv_mutex_t* mutex);
static int uv_cond_fallback_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout);

static int uv_cond_condvar_init(uv_cond_t* cond);
static void uv_cond_condvar_destroy(uv_cond_t* cond);
static void uv_cond_condvar_signal(uv_cond_t* cond);
static void uv_cond_condvar_broadcast(uv_cond_t* cond);
static void uv_cond_condvar_wait(uv_cond_t* cond, uv_mutex_t* mutex);
static int uv_cond_condvar_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout);


static void uv__once_inner(uv_once_t* guard, void (*callback)(void)) {
  DWORD result;
  HANDLE existing_event, created_event;

  created_event = CreateEvent(NULL, 1, 0, NULL);
  if (created_event == 0) {
    /* Could fail in a low-memory situation? */
    uv_fatal_error(GetLastError(), "CreateEvent");
  }

  existing_event = InterlockedCompareExchangePointer(&guard->event,
                                                     created_event,
                                                     NULL);

  if (existing_event == NULL) {
    /* We won the race */
    callback();

    result = SetEvent(created_event);
    assert(result);
    guard->ran = 1;

  } else {
    /* We lost the race. Destroy the event we created and wait for the */
    /* existing one to become signaled. */
    CloseHandle(created_event);
    result = WaitForSingleObject(existing_event, INFINITE);
    assert(result == WAIT_OBJECT_0);
  }
}


void uv_once(uv_once_t* guard, void (*callback)(void)) {
  /* Fast case - avoid WaitForSingleObject. */
  if (guard->ran) {
    return;
  }

  uv__once_inner(guard, callback);
}


/* Verify that uv_thread_t can be stored in a TLS slot. */
STATIC_ASSERT(sizeof(uv_thread_t) <= sizeof(void*));

static uv_key_t uv__current_thread_key;
static uv_once_t uv__current_thread_init_guard = UV_ONCE_INIT;


static void uv__init_current_thread_key(void) {
  if (uv_key_create(&uv__current_thread_key))
    abort();
}


struct thread_ctx {
  void (*entry)(void* arg);
  void* arg;
  uv_thread_t self;
};


static UINT __stdcall uv__thread_start(void* arg) {
  struct thread_ctx *ctx_p;
  struct thread_ctx ctx;

  ctx_p = arg;
  ctx = *ctx_p;
  free(ctx_p);

  uv_once(&uv__current_thread_init_guard, uv__init_current_thread_key);
  uv_key_set(&uv__current_thread_key, (void*) ctx.self);

  ctx.entry(ctx.arg);

  return 0;
}

#define USE_USER_MODE_SCHEDUING
#ifdef USE_USER_MODE_SCHEDUING
#define UMS_NUM_THREADS_PER_SCHEDULER 8 //need to pick a better number probably

#if 1 /* These items should be included from Windows.h */
typedef void *PUMS_COMPLETION_LIST;
typedef void *PUMS_CONTEXT;
#define RTL_UMS_VERSION  (0x0100) 
#define UMS_VERSION RTL_UMS_VERSION
#define PROC_THREAD_ATTRIBUTE_UMS_THREAD ProcThreadAttributeValue (6, TRUE, TRUE, FALSE)

typedef enum _RTL_UMS_SCHEDULER_REASON UMS_SCHEDULER_REASON;

typedef struct _UMS_THREAD_RING {
  PUMS_CONTEXT contextRing[UMS_NUM_THREADS_PER_SCHEDULER];
  int readCount;
  int writeCount;
} UMS_THREAD_RING, *PUMS_THREAD_RING;

typedef PRTL_UMS_SCHEDULER_ENTRY_POINT PUMS_SCHEDULER_ENTRY_POINT;

typedef struct _UMS_SCHEDULER_STARTUP_INFO {
  ULONG                      UmsVersion;
  PUMS_COMPLETION_LIST       CompletionList;
  PUMS_SCHEDULER_ENTRY_POINT SchedulerProc;
  PVOID                      SchedulerParam;
} UMS_SCHEDULER_STARTUP_INFO, *PUMS_SCHEDULER_STARTUP_INFO;

typedef struct _UMS_SCHEDULER_CONTEXT {
  UMS_SCHEDULER_STARTUP_INFO startupInfo;
  HANDLE completionListEvent;
  UMS_THREAD_RING threadRing;
} UMS_SCHEDULER_CONTEXT, *PUMS_SCHEDULER_CONTEXT;

typedef struct _UMS_SYSTEM_THREAD_INFORMATION {
    ULONG UmsVersion;
    union {
        struct {
            ULONG IsUmsSchedulerThread : 1;
            ULONG IsUmsWorkerThread : 1;
        } DUMMYSTRUCTNAME;
        ULONG ThreadUmsFlags;
    } DUMMYUNIONNAME;
} UMS_SYSTEM_THREAD_INFORMATION, *PUMS_SYSTEM_THREAD_INFORMATION;

typedef enum _RTL_UMS_THREAD_INFO_CLASS UMS_THREAD_INFO_CLASS, *PUMS_THREAD_INFO_CLASS;

WINBASEAPI BOOL WINAPI CreateUmsCompletionList(_Outptr_ PUMS_COMPLETION_LIST* UmsCompletionList);
WINBASEAPI BOOL WINAPI DequeueUmsCompletionListItems(_In_ PUMS_COMPLETION_LIST UmsCompletionList, _In_ DWORD WaitTimeOut, _Out_ PUMS_CONTEXT* UmsThreadList);
WINBASEAPI BOOL WINAPI GetUmsCompletionListEvent(_In_ PUMS_COMPLETION_LIST UmsCompletionList, _Inout_ PHANDLE UmsCompletionEvent);
WINBASEAPI BOOL WINAPI ExecuteUmsThread(_Inout_ PUMS_CONTEXT UmsThread);
WINBASEAPI BOOL WINAPI UmsThreadYield(_In_ PVOID SchedulerParam);
WINBASEAPI BOOL WINAPI DeleteUmsCompletionList(_In_ PUMS_COMPLETION_LIST UmsCompletionList);
WINBASEAPI PUMS_CONTEXT WINAPI GetCurrentUmsThread(VOID);
WINBASEAPI PUMS_CONTEXT WINAPI GetNextUmsListItem(_Inout_ PUMS_CONTEXT UmsContext);
WINBASEAPI BOOL WINAPI QueryUmsThreadInformation(_In_ PUMS_CONTEXT UmsThread, _In_ UMS_THREAD_INFO_CLASS UmsThreadInfoClass, _Out_writes_bytes_to_(UmsThreadInformationLength, *ReturnLength) PVOID UmsThreadInformation, _In_ ULONG UmsThreadInformationLength, _Out_opt_ PULONG ReturnLength);
WINBASEAPI BOOL WINAPI SetUmsThreadInformation(_In_ PUMS_CONTEXT UmsThread, _In_ UMS_THREAD_INFO_CLASS UmsThreadInfoClass, _In_ PVOID UmsThreadInformation, _In_ ULONG UmsThreadInformationLength);
WINBASEAPI BOOL WINAPI DeleteUmsThreadContext(_In_ PUMS_CONTEXT UmsThread);
WINBASEAPI BOOL WINAPI CreateUmsThreadContext(_Outptr_ PUMS_CONTEXT *lpUmsThread);
WINBASEAPI BOOL WINAPI EnterUmsSchedulingMode(_In_ PUMS_SCHEDULER_STARTUP_INFO SchedulerStartupInfo);
WINBASEAPI BOOL WINAPI GetUmsSystemThreadInformation(_In_ HANDLE ThreadHandle, _Inout_ PUMS_SYSTEM_THREAD_INFORMATION SystemThreadInfo);
#endif /* These items should be included from Windows.h */

HANDLE begin_ums_worker_thread(PUMS_COMPLETION_LIST CompletionList, unsigned ( __stdcall *start_address )( void * ), void *arglist, unsigned initflag) {
  SIZE_T lpSize=0;
  PUMS_CONTEXT tContext=0;
  CreateUmsThreadContext(&tContext);

  LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList = NULL;

  InitializeProcThreadAttributeList(lpAttributeList, 1, 0, &lpSize);
  lpAttributeList=(LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), HEAP_GENERATE_EXCEPTIONS|HEAP_ZERO_MEMORY, lpSize);
  InitializeProcThreadAttributeList(lpAttributeList, 1, 0, &lpSize);

  UMS_CREATE_THREAD_ATTRIBUTES threadAttributes;
  threadAttributes.UmsVersion=UMS_VERSION;
  threadAttributes.UmsContext=tContext;
  threadAttributes.UmsCompletionList=CompletionList;

  UpdateProcThreadAttribute(lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_UMS_THREAD, &threadAttributes, sizeof(threadAttributes), NULL, NULL);

  HANDLE rt1=CreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, start_address, arglist, initflag, lpAttributeList, NULL);

  DeleteProcThreadAttributeList(lpAttributeList);

  HeapFree(GetProcessHeap(), 0, lpAttributeList); lpAttributeList=NULL;

  return rt1;
}

void addToThreadRing(PUMS_CONTEXT UmsThreadContext, PUMS_THREAD_RING threadRing) {
  // not thread safe, each scheduler thread has its own
  // can be made thread safe with critial section, or lock, etc
  threadRing->contextRing[(threadRing->writeCount++) % UMS_NUM_THREADS_PER_SCHEDULER] = UmsThreadContext;
}

PUMS_CONTEXT removeFromThreadRing(PUMS_THREAD_RING threadRing) {
  // not thread safe, each scheduler thread has its own
  // can be made thread safe with critial section, or lock, etc
  if(threadRing->readCount < threadRing->writeCount) {
    return threadRing->contextRing[(threadRing->readCount++) % UMS_NUM_THREADS_PER_SCHEDULER];
  } else {
    return NULL;
  }
}

void dequeueToThreadRing(PUMS_COMPLETION_LIST CompletionList, PUMS_THREAD_RING threadRing) {
  PUMS_CONTEXT UmsThreadList = NULL;
  if(!DequeueUmsCompletionListItems(CompletionList, INFINITE, &UmsThreadList)) return;
  while(UmsThreadList) {
    addToThreadRing(UmsThreadList, threadRing);
    UmsThreadList = GetNextUmsListItem(UmsThreadList);
  }
}

void schedulerCallback(UMS_SCHEDULER_REASON Reason, ULONG_PTR ActivationPayload, PVOID SchedulerParam, PUMS_SCHEDULER_CONTEXT schedulerContext) {
/*
  Called when scheduler starts, a thread blocks, or a thread yields.
  See UmsSchedulerProc callback function https://msdn.microsoft.com/en-us/library/windows/desktop/dd627182(v=vs.85).aspx
*/

/*
  switch(Reason) {
  case UmsSchedulerStartup:       //TODO do some initialization
  case UmsSchedulerThreadBlocked: //TODO do something
  case UmsSchedulerThreadYield:   //TODO put into specific yield queue
  }
*/

  for(;;) {
    PUMS_CONTEXT threadContext = removeFromThreadRing(&schedulerContext->threadRing);
    if(threadContext) {
      ExecuteUmsThread(threadContext);
      //TODO check for status and handle it
    }

    DWORD waitStatus = WaitForSingleObjectEx(schedulerContext->completionListEvent, INFINITE, TRUE);

    switch(waitStatus) {
    case WAIT_OBJECT_0:
      dequeueToThreadRing(schedulerContext->startupInfo.CompletionList, &schedulerContext->threadRing);
      break;

    case WAIT_ABANDONED: //TODO handle error cases
      DebugBreak();
      break;

    case WAIT_IO_COMPLETION: //TODO handle error cases
      DebugBreak();
      break;

    case WAIT_TIMEOUT: //TODO handle error cases
      DebugBreak();
      break;

    case WAIT_FAILED: //TODO handle error cases
      DebugBreak();
      break;

    default: //TODO handle error cases
      DebugBreak();
      break;
    }
  }
  DebugBreak(); //TODO handle error cases
}

void *x64_bind_4th_arg(void *proc, void *fouthArg) {
  /*
    Quick and dirty attempt to replace C++ funtional binding in plain C, using the binary of x64 assembly.
    Please let me know if there is a proper way to do this. But, it was faster to do this than find a better solution for now.
  */

  /*
    [offset] [      bytes       ]    [          instruction          ]
    00000000 48B8_____proc_______    mov rax, qword 0x_____proc_______
    0000000A 49B9____fouthArg____    mov  r9, qword 0x____fouthArg____
    00000014 FFE0                    jmp rax
  */

  unsigned char *callPtr = VirtualAlloc(0, 22, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  callPtr[0x00] = 0x48;
  callPtr[0x01] = 0xB8;
  *(void * *)(callPtr + 0x02) = proc;

  callPtr[0x0A] = 0x49;
  callPtr[0x0B] = 0xB9;
  *(void * *)(callPtr + 0x0C) = fouthArg;

  callPtr[0x14] = 0xFF;
  callPtr[0x15] = 0xE0;

  DWORD oldProtect = 0;
  VirtualProtect(callPtr, 22, PAGE_EXECUTE_READ, &oldProtect);
  return callPtr;
}

DWORD scheduler(struct thread_ctx* sCtx) {
  UMS_SCHEDULER_CONTEXT schedulerContext;
  SecureZeroMemory(&schedulerContext, sizeof(schedulerContext));
  schedulerContext.startupInfo.UmsVersion = UMS_VERSION;
  schedulerContext.startupInfo.CompletionList = 0;
  schedulerContext.startupInfo.SchedulerProc = x64_bind_4th_arg(schedulerCallback, &schedulerContext);
  schedulerContext.startupInfo.SchedulerParam = &schedulerContext;

  CreateUmsCompletionList(&schedulerContext.startupInfo.CompletionList);
  GetUmsCompletionListEvent(schedulerContext.startupInfo.CompletionList, &schedulerContext.completionListEvent);

  int i;
  for(i = 0; i < 8; i++) {
    struct thread_ctx* ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
      return UV_ENOMEM;

    ctx->entry = sCtx->entry;
    ctx->arg = sCtx->arg;

    CloseHandle(begin_ums_worker_thread(schedulerContext.startupInfo.CompletionList, uv__thread_start, ctx, 0));
  }
  free(sCtx);

  EnterUmsSchedulingMode(&schedulerContext.startupInfo);
  DeleteUmsCompletionList(&schedulerContext.startupInfo);
  return 0;
}

HANDLE begin_ums_scheduler_thread(void *arglist, unsigned initflag) {
  return CreateThread(0, 0, scheduler, arglist, initflag, 0);
}

#endif

int uv_thread_create(uv_thread_t *tid, void (*entry)(void *arg), void *arg) {
  struct thread_ctx* ctx;
  int err;
  HANDLE thread;

  ctx = malloc(sizeof(*ctx));
  if (ctx == NULL)
    return UV_ENOMEM;

  ctx->entry = entry;
  ctx->arg = arg;

#ifdef USE_USER_MODE_SCHEDUING
  thread = begin_ums_scheduler_thread(ctx, CREATE_SUSPENDED);
#else
  /* Create the thread in suspended state so we have a chance to pass
   * its own creation handle to it */   
  thread = (HANDLE) _beginthreadex(NULL,
                                   0,
                                   uv__thread_start,
                                   ctx,
                                   CREATE_SUSPENDED,
                                   NULL);
#endif
  if (thread == NULL) {
    err = errno;
    free(ctx);
  } else {
    err = 0;
    *tid = thread;
    ctx->self = thread;
    ResumeThread(thread);
  }

  switch (err) {
    case 0:
      return 0;
    case EACCES:
      return UV_EACCES;
    case EAGAIN:
      return UV_EAGAIN;
    case EINVAL:
      return UV_EINVAL;
  }

  return UV_EIO;
}


uv_thread_t uv_thread_self(void) {
  return (uv_thread_t) uv_key_get(&uv__current_thread_key);
}


int uv_thread_join(uv_thread_t *tid) {
  if (WaitForSingleObject(*tid, INFINITE))
    return uv_translate_sys_error(GetLastError());
  else {
    CloseHandle(*tid);
    *tid = 0;
    return 0;
  }
}


int uv_thread_equal(const uv_thread_t* t1, const uv_thread_t* t2) {
  return *t1 == *t2;
}


int uv_mutex_init(uv_mutex_t* mutex) {
  InitializeCriticalSection(mutex);
  return 0;
}


void uv_mutex_destroy(uv_mutex_t* mutex) {
  DeleteCriticalSection(mutex);
}


void uv_mutex_lock(uv_mutex_t* mutex) {
  EnterCriticalSection(mutex);
}


int uv_mutex_trylock(uv_mutex_t* mutex) {
  if (TryEnterCriticalSection(mutex))
    return 0;
  else
    return UV_EAGAIN;
}


void uv_mutex_unlock(uv_mutex_t* mutex) {
  LeaveCriticalSection(mutex);
}


int uv_rwlock_init(uv_rwlock_t* rwlock) {
  uv__once_init();

  if (HAVE_SRWLOCK_API())
    return uv__rwlock_srwlock_init(rwlock);
  else
    return uv__rwlock_fallback_init(rwlock);
}


void uv_rwlock_destroy(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_destroy(rwlock);
  else
    uv__rwlock_fallback_destroy(rwlock);
}


void uv_rwlock_rdlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_rdlock(rwlock);
  else
    uv__rwlock_fallback_rdlock(rwlock);
}


int uv_rwlock_tryrdlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    return uv__rwlock_srwlock_tryrdlock(rwlock);
  else
    return uv__rwlock_fallback_tryrdlock(rwlock);
}


void uv_rwlock_rdunlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_rdunlock(rwlock);
  else
    uv__rwlock_fallback_rdunlock(rwlock);
}


void uv_rwlock_wrlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_wrlock(rwlock);
  else
    uv__rwlock_fallback_wrlock(rwlock);
}


int uv_rwlock_trywrlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    return uv__rwlock_srwlock_trywrlock(rwlock);
  else
    return uv__rwlock_fallback_trywrlock(rwlock);
}


void uv_rwlock_wrunlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_wrunlock(rwlock);
  else
    uv__rwlock_fallback_wrunlock(rwlock);
}


int uv_sem_init(uv_sem_t* sem, unsigned int value) {
  *sem = CreateSemaphore(NULL, value, INT_MAX, NULL);
  if (*sem == NULL)
    return uv_translate_sys_error(GetLastError());
  else
    return 0;
}


void uv_sem_destroy(uv_sem_t* sem) {
  if (!CloseHandle(*sem))
    abort();
}


void uv_sem_post(uv_sem_t* sem) {
  if (!ReleaseSemaphore(*sem, 1, NULL))
    abort();
}


void uv_sem_wait(uv_sem_t* sem) {
  if (WaitForSingleObject(*sem, INFINITE) != WAIT_OBJECT_0)
    abort();
}


int uv_sem_trywait(uv_sem_t* sem) {
  DWORD r = WaitForSingleObject(*sem, 0);

  if (r == WAIT_OBJECT_0)
    return 0;

  if (r == WAIT_TIMEOUT)
    return UV_EAGAIN;

  abort();
  return -1; /* Satisfy the compiler. */
}


static int uv__rwlock_srwlock_init(uv_rwlock_t* rwlock) {
  pInitializeSRWLock(&rwlock->srwlock_);
  return 0;
}


static void uv__rwlock_srwlock_destroy(uv_rwlock_t* rwlock) {
  (void) rwlock;
}


static void uv__rwlock_srwlock_rdlock(uv_rwlock_t* rwlock) {
  pAcquireSRWLockShared(&rwlock->srwlock_);
}


static int uv__rwlock_srwlock_tryrdlock(uv_rwlock_t* rwlock) {
  if (pTryAcquireSRWLockShared(&rwlock->srwlock_))
    return 0;
  else
    return UV_EBUSY;  /* TODO(bnoordhuis) EAGAIN when owned by this thread. */
}


static void uv__rwlock_srwlock_rdunlock(uv_rwlock_t* rwlock) {
  pReleaseSRWLockShared(&rwlock->srwlock_);
}


static void uv__rwlock_srwlock_wrlock(uv_rwlock_t* rwlock) {
  pAcquireSRWLockExclusive(&rwlock->srwlock_);
}


static int uv__rwlock_srwlock_trywrlock(uv_rwlock_t* rwlock) {
  if (pTryAcquireSRWLockExclusive(&rwlock->srwlock_))
    return 0;
  else
    return UV_EBUSY;  /* TODO(bnoordhuis) EAGAIN when owned by this thread. */
}


static void uv__rwlock_srwlock_wrunlock(uv_rwlock_t* rwlock) {
  pReleaseSRWLockExclusive(&rwlock->srwlock_);
}


static int uv__rwlock_fallback_init(uv_rwlock_t* rwlock) {
  int err;

  err = uv_mutex_init(&rwlock->fallback_.read_mutex_);
  if (err)
    return err;

  err = uv_mutex_init(&rwlock->fallback_.write_mutex_);
  if (err) {
    uv_mutex_destroy(&rwlock->fallback_.read_mutex_);
    return err;
  }

  rwlock->fallback_.num_readers_ = 0;

  return 0;
}


static void uv__rwlock_fallback_destroy(uv_rwlock_t* rwlock) {
  uv_mutex_destroy(&rwlock->fallback_.read_mutex_);
  uv_mutex_destroy(&rwlock->fallback_.write_mutex_);
}


static void uv__rwlock_fallback_rdlock(uv_rwlock_t* rwlock) {
  uv_mutex_lock(&rwlock->fallback_.read_mutex_);

  if (++rwlock->fallback_.num_readers_ == 1)
    uv_mutex_lock(&rwlock->fallback_.write_mutex_);

  uv_mutex_unlock(&rwlock->fallback_.read_mutex_);
}


static int uv__rwlock_fallback_tryrdlock(uv_rwlock_t* rwlock) {
  int err;

  err = uv_mutex_trylock(&rwlock->fallback_.read_mutex_);
  if (err)
    goto out;

  err = 0;
  if (rwlock->fallback_.num_readers_ == 0)
    err = uv_mutex_trylock(&rwlock->fallback_.write_mutex_);

  if (err == 0)
    rwlock->fallback_.num_readers_++;

  uv_mutex_unlock(&rwlock->fallback_.read_mutex_);

out:
  return err;
}


static void uv__rwlock_fallback_rdunlock(uv_rwlock_t* rwlock) {
  uv_mutex_lock(&rwlock->fallback_.read_mutex_);

  if (--rwlock->fallback_.num_readers_ == 0)
    uv_mutex_unlock(&rwlock->fallback_.write_mutex_);

  uv_mutex_unlock(&rwlock->fallback_.read_mutex_);
}


static void uv__rwlock_fallback_wrlock(uv_rwlock_t* rwlock) {
  uv_mutex_lock(&rwlock->fallback_.write_mutex_);
}


static int uv__rwlock_fallback_trywrlock(uv_rwlock_t* rwlock) {
  return uv_mutex_trylock(&rwlock->fallback_.write_mutex_);
}


static void uv__rwlock_fallback_wrunlock(uv_rwlock_t* rwlock) {
  uv_mutex_unlock(&rwlock->fallback_.write_mutex_);
}



/* This condition variable implementation is based on the SetEvent solution
 * (section 3.2) at http://www.cs.wustl.edu/~schmidt/win32-cv-1.html
 * We could not use the SignalObjectAndWait solution (section 3.4) because
 * it want the 2nd argument (type uv_mutex_t) of uv_cond_wait() and
 * uv_cond_timedwait() to be HANDLEs, but we use CRITICAL_SECTIONs.
 */

static int uv_cond_fallback_init(uv_cond_t* cond) {
  int err;

  /* Initialize the count to 0. */
  cond->fallback.waiters_count = 0;

  InitializeCriticalSection(&cond->fallback.waiters_count_lock);

  /* Create an auto-reset event. */
  cond->fallback.signal_event = CreateEvent(NULL,  /* no security */
                                            FALSE, /* auto-reset event */
                                            FALSE, /* non-signaled initially */
                                            NULL); /* unnamed */
  if (!cond->fallback.signal_event) {
    err = GetLastError();
    goto error2;
  }

  /* Create a manual-reset event. */
  cond->fallback.broadcast_event = CreateEvent(NULL,  /* no security */
                                               TRUE,  /* manual-reset */
                                               FALSE, /* non-signaled */
                                               NULL); /* unnamed */
  if (!cond->fallback.broadcast_event) {
    err = GetLastError();
    goto error;
  }

  return 0;

error:
  CloseHandle(cond->fallback.signal_event);
error2:
  DeleteCriticalSection(&cond->fallback.waiters_count_lock);
  return uv_translate_sys_error(err);
}


static int uv_cond_condvar_init(uv_cond_t* cond) {
  pInitializeConditionVariable(&cond->cond_var);
  return 0;
}


int uv_cond_init(uv_cond_t* cond) {
  uv__once_init();

  if (HAVE_CONDVAR_API())
    return uv_cond_condvar_init(cond);
  else
    return uv_cond_fallback_init(cond);
}


static void uv_cond_fallback_destroy(uv_cond_t* cond) {
  if (!CloseHandle(cond->fallback.broadcast_event))
    abort();
  if (!CloseHandle(cond->fallback.signal_event))
    abort();
  DeleteCriticalSection(&cond->fallback.waiters_count_lock);
}


static void uv_cond_condvar_destroy(uv_cond_t* cond) {
  /* nothing to do */
}


void uv_cond_destroy(uv_cond_t* cond) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_destroy(cond);
  else
    uv_cond_fallback_destroy(cond);
}


static void uv_cond_fallback_signal(uv_cond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  have_waiters = cond->fallback.waiters_count > 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->fallback.signal_event);
}


static void uv_cond_condvar_signal(uv_cond_t* cond) {
  pWakeConditionVariable(&cond->cond_var);
}


void uv_cond_signal(uv_cond_t* cond) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_signal(cond);
  else
    uv_cond_fallback_signal(cond);
}


static void uv_cond_fallback_broadcast(uv_cond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  have_waiters = cond->fallback.waiters_count > 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->fallback.broadcast_event);
}


static void uv_cond_condvar_broadcast(uv_cond_t* cond) {
  pWakeAllConditionVariable(&cond->cond_var);
}


void uv_cond_broadcast(uv_cond_t* cond) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_broadcast(cond);
  else
    uv_cond_fallback_broadcast(cond);
}


static int uv_cond_wait_helper(uv_cond_t* cond, uv_mutex_t* mutex,
    DWORD dwMilliseconds) {
  DWORD result;
  int last_waiter;
  HANDLE handles[2] = {
    cond->fallback.signal_event,
    cond->fallback.broadcast_event
  };

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  cond->fallback.waiters_count++;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  /* It's ok to release the <mutex> here since Win32 manual-reset events */
  /* maintain state when used with <SetEvent>. This avoids the "lost wakeup" */
  /* bug. */
  uv_mutex_unlock(mutex);

  /* Wait for either event to become signaled due to <uv_cond_signal> being */
  /* called or <uv_cond_broadcast> being called. */
  result = WaitForMultipleObjects(2, handles, FALSE, dwMilliseconds);

  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  cond->fallback.waiters_count--;
  last_waiter = result == WAIT_OBJECT_0 + 1
      && cond->fallback.waiters_count == 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  /* Some thread called <pthread_cond_broadcast>. */
  if (last_waiter) {
    /* We're the last waiter to be notified or to stop waiting, so reset the */
    /* the manual-reset event. */
    ResetEvent(cond->fallback.broadcast_event);
  }

  /* Reacquire the <mutex>. */
  uv_mutex_lock(mutex);

  if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1)
    return 0;

  if (result == WAIT_TIMEOUT)
    return UV_ETIMEDOUT;

  abort();
  return -1; /* Satisfy the compiler. */
}


static void uv_cond_fallback_wait(uv_cond_t* cond, uv_mutex_t* mutex) {
  if (uv_cond_wait_helper(cond, mutex, INFINITE))
    abort();
}


static void uv_cond_condvar_wait(uv_cond_t* cond, uv_mutex_t* mutex) {
  if (!pSleepConditionVariableCS(&cond->cond_var, mutex, INFINITE))
    abort();
}


void uv_cond_wait(uv_cond_t* cond, uv_mutex_t* mutex) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_wait(cond, mutex);
  else
    uv_cond_fallback_wait(cond, mutex);
}


static int uv_cond_fallback_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout) {
  return uv_cond_wait_helper(cond, mutex, (DWORD)(timeout / 1e6));
}


static int uv_cond_condvar_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout) {
  if (pSleepConditionVariableCS(&cond->cond_var, mutex, (DWORD)(timeout / 1e6)))
    return 0;
  if (GetLastError() != ERROR_TIMEOUT)
    abort();
  return UV_ETIMEDOUT;
}


int uv_cond_timedwait(uv_cond_t* cond, uv_mutex_t* mutex,
    uint64_t timeout) {
  if (HAVE_CONDVAR_API())
    return uv_cond_condvar_timedwait(cond, mutex, timeout);
  else
    return uv_cond_fallback_timedwait(cond, mutex, timeout);
}


int uv_barrier_init(uv_barrier_t* barrier, unsigned int count) {
  int err;

  barrier->n = count;
  barrier->count = 0;

  err = uv_mutex_init(&barrier->mutex);
  if (err)
    return err;

  err = uv_sem_init(&barrier->turnstile1, 0);
  if (err)
    goto error2;

  err = uv_sem_init(&barrier->turnstile2, 1);
  if (err)
    goto error;

  return 0;

error:
  uv_sem_destroy(&barrier->turnstile1);
error2:
  uv_mutex_destroy(&barrier->mutex);
  return err;

}


void uv_barrier_destroy(uv_barrier_t* barrier) {
  uv_sem_destroy(&barrier->turnstile2);
  uv_sem_destroy(&barrier->turnstile1);
  uv_mutex_destroy(&barrier->mutex);
}


int uv_barrier_wait(uv_barrier_t* barrier) {
  int serial_thread;

  uv_mutex_lock(&barrier->mutex);
  if (++barrier->count == barrier->n) {
    uv_sem_wait(&barrier->turnstile2);
    uv_sem_post(&barrier->turnstile1);
  }
  uv_mutex_unlock(&barrier->mutex);

  uv_sem_wait(&barrier->turnstile1);
  uv_sem_post(&barrier->turnstile1);

  uv_mutex_lock(&barrier->mutex);
  serial_thread = (--barrier->count == 0);
  if (serial_thread) {
    uv_sem_wait(&barrier->turnstile1);
    uv_sem_post(&barrier->turnstile2);
  }
  uv_mutex_unlock(&barrier->mutex);

  uv_sem_wait(&barrier->turnstile2);
  uv_sem_post(&barrier->turnstile2);
  return serial_thread;
}


int uv_key_create(uv_key_t* key) {
  key->tls_index = TlsAlloc();
  if (key->tls_index == TLS_OUT_OF_INDEXES)
    return UV_ENOMEM;
  return 0;
}


void uv_key_delete(uv_key_t* key) {
  if (TlsFree(key->tls_index) == FALSE)
    abort();
  key->tls_index = TLS_OUT_OF_INDEXES;
}


void* uv_key_get(uv_key_t* key) {
  void* value;

  value = TlsGetValue(key->tls_index);
  if (value == NULL)
    if (GetLastError() != ERROR_SUCCESS)
      abort();

  return value;
}


void uv_key_set(uv_key_t* key, void* value) {
  if (TlsSetValue(key->tls_index, value) == FALSE)
    abort();
}
