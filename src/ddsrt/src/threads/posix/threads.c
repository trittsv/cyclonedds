// Copyright(c) 2006 to 2022 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause


/* _GNU_SOURCE is required for pthread_getname_np and pthread_setname_np. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>

#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include <limits.h>

#include "threads_priv.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/log.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/types.h"
#include "dds/ddsrt/static_assert.h"
#include "dds/ddsrt/misc.h"

typedef struct {
  char *name;
  ddsrt_thread_routine_t routine;
  void *arg;
} thread_context_t;

#if defined(__linux)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <dirent.h>
#define MAXTHREADNAMESIZE (15) /* 16 bytes including null-terminating byte. */
#elif defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/thread_info.h> /* MAXTHREADNAMESIZE */
#include <mach/task.h>
#include <mach/task_info.h>
#include <mach/vm_map.h>
#include <AvailabilityMacros.h>
#ifndef MAXTHREADNAMESIZE
#define MAXTHREADNAMESIZE (64)
#endif /* MAXTHREADNAMESIZE */
#elif defined(__sun)
#define MAXTHREADNAMESIZE (31)
#elif defined(__FreeBSD__)
/* Required for pthread_get_name_np and pthread_set_name_np. */
#include <pthread_np.h>
#include <sys/thr.h>
#define MAXTHREADNAMESIZE (MAXCOMLEN)
#elif defined(__VXWORKS__)
#include <taskLib.h>
/* VX_TASK_NAME_LENGTH is the maximum number of bytes, excluding
   null-terminating byte, for a thread name. */
#define MAXTHREADNAMESIZE (VX_TASK_NAME_LENGTH)
#elif defined(__QNXNTO__)
#include <pthread.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#define MAXTHREADNAMESIZE (_NTO_THREAD_NAME_MAX - 1)
#elif defined(__ZEPHYR__) && defined(CONFIG_THREAD_NAME)
/* CONFIG_THREAD_MAX_NAME_LEN indicates the max name length,
   including the terminating NULL byte */
#define MAXTHREADNAMESIZE (CONFIG_THREAD_MAX_NAME_LEN - 1)
#endif /* __APPLE__ */

#if defined(__ZEPHYR__) && !defined(CONFIG_FILE_SYSTEM)
int _open(const char *name, int mode);
int _open(const char *name, int mode) { return -1; }
#endif

size_t
ddsrt_thread_getname(char *name, size_t size)
{
#ifdef MAXTHREADNAMESIZE
  char buf[MAXTHREADNAMESIZE + 1] = "";
#endif
  size_t cnt = 0;

  assert(name != NULL);
  assert(size > 0);

#if defined(__linux)
  /* Thread names are limited to 16 bytes on Linux, which the buffer should
     allow space for. prctl is favored over pthread_getname_np for
     portability. e.g. musl libc. */
  (void)prctl(PR_GET_NAME, (unsigned long)buf, 0UL, 0UL, 0UL);
  cnt = ddsrt_strlcpy(name, buf, size);
#elif defined(__APPLE__)
  /* pthread_getname_np on APPLE uses strlcpy to copy the thread name, but
     does not return the number of bytes (that would have been) written. Use
     an intermediate buffer. */
  (void)pthread_getname_np(pthread_self(), buf, sizeof(buf));
  cnt = ddsrt_strlcpy(name, buf, size);
#elif defined(__FreeBSD__)
  (void)pthread_get_name_np(pthread_self(), buf, sizeof(buf));
  cnt = ddsrt_strlcpy(name, buf, size);
#elif defined(__sun)
#if !(__SunOS_5_6 || __SunOS_5_7 || __SunOS_5_8 || __SunOS_5_9 || __SunOS_5_10)
  (void)pthread_getname_np(pthread_self(), buf, sizeof(buf));
#else
  buf[0] = 0;
#endif
  cnt = ddsrt_strlcpy(name, buf, size);
#elif defined(__VXWORKS__)
  {
    char *ptr;
    /* VxWorks does not support retrieving the name of a task through the
       POSIX thread API, but the task API offers it through taskName. */
    /* Do not free the pointer returned by taskName. See
       src/wind/taskInfo.c for details. */
    ptr = taskName(taskIdSelf());
    if (ptr == NULL) {
      ptr = buf;
    }
    cnt = ddsrt_strlcpy(name, ptr, size);
  }
#elif defined(__QNXNTO__)
  (void)pthread_getname_np(pthread_self(), buf, sizeof(buf));
  cnt = ddsrt_strlcpy(name, buf, size);
#elif defined(__ZEPHYR__) && defined(CONFIG_THREAD_NAME)
  (void)pthread_getname_np(pthread_self(), buf, sizeof(buf));
  cnt = ddsrt_strlcpy(name, buf, size);
#endif

  /* Thread identifier is used as fall back if thread name lookup is not
     supported or the thread name is empty. */
  if (cnt == 0) {
    ddsrt_tid_t tid = ddsrt_gettid();
    cnt = (size_t)snprintf(name, size, "%"PRIdTID, tid);
  }

  return cnt;
}

void
ddsrt_thread_setname(const char *name)
{
  assert(name != NULL);

#if defined(__linux)
  /* Thread names are limited to 16 bytes on Linux. ERANGE is returned if the
     name exceeds the limit, so silently truncate. */
  char buf[MAXTHREADNAMESIZE + 1] = "";
  (void)ddsrt_strlcpy(buf, name, sizeof(buf));
  (void)pthread_setname_np(pthread_self(), buf);
#elif defined(__APPLE__)
  (void)pthread_setname_np(name);
#elif defined(__FreeBSD__)
  (void)pthread_set_name_np(pthread_self(), name);
#elif defined(__sun)
  /* Thread names are limited to 31 bytes on Solaris. Excess bytes are
     silently truncated. */
#if !(__SunOS_5_6 || __SunOS_5_7 || __SunOS_5_8 || __SunOS_5_9 || __SunOS_5_10)
  (void)pthread_setname_np(pthread_self(), name);
#endif
#elif defined(__QNXNTO__)
  (void)pthread_setname_np(pthread_self(), name);
#elif defined(__VXWORKS__)
  (void)pthread_setname_np(pthread_self(), name);
#elif defined(__ZEPHYR__) && defined(CONFIG_THREAD_NAME)
  (void)pthread_setname_np(pthread_self(), (char*)name);
#else
  /* VxWorks does not support the task name to be set after a task is created.
     Setting the name of a task can be done through pthread_attr_setname. */
#warning "ddsrt_thread_setname is not supported"
#endif
}

/** \brief Wrap thread start routine
 *
 * \b os_startRoutineWrapper wraps a threads starting routine.
 * before calling the user routine, it sets the threads name
 * in the context of the thread. With \b pthread_getspecific,
 * the name can be retreived for different purposes.
 */
static void *os_startRoutineWrapper (void *threadContext)
{
  thread_context_t *context = threadContext;
  uintptr_t resultValue;

  ddsrt_thread_setname(context->name);

  /* Call the user routine */
  resultValue = context->routine (context->arg);

  /* Free the thread context resources, arguments is responsibility */
  /* for the caller of os_procCreate                                */
  ddsrt_free(context->name);
  ddsrt_free(context);

#if defined(__VXWORKS__) && !defined(_WRS_KERNEL)
  struct sched_param sched_param;
  int max, policy = 0;

  /* There is a known issue in pthread_join on VxWorks 6.x RTP mode.

     WindRiver: When pthread_join returns, it does not indicate end of a
     thread in 100% of the situations. If the thread that calls pthread_join
     has a higher priority than the thread that is currently terminating,
     pthread_join could return before pthread_exit has finished. This
     conflicts with the POSIX specification that dictates that pthread_join
     must only return when the thread is really terminated. The workaround
     suggested by WindRiver support is to increase the priority of the thread
     (task) to be terminated before handing back the semaphore to ensure the
     thread exits before pthread_join returns.

     This bug was submitted to WindRiver as TSR 815826. */

  /* Note that any possible errors raised here are not terminal since the
     thread may have exited at this point anyway. */
  if (pthread_getschedparam(pthread_self (), &policy, &sched_param) == 0) {
    max = sched_get_priority_max(policy);
    if (max != -1) {
      (void)pthread_setschedprio(pthread_self (), max);
    }
  }
#endif

  /* return the result of the user routine */
  return (void *)resultValue;
}

#if defined(__ZEPHYR__)
#ifndef CYCLONEDDS_THREAD_COUNT 
#define CYCLONEDDS_THREAD_COUNT 10
#endif

#ifndef CYCLONEDDS_THREAD_STACK_SIZE
#define CYCLONEDDS_THREAD_STACK_SIZE 32768
#endif

#if (CYCLONEDDS_THREAD_COUNT > CONFIG_MAX_PTHREAD_COUNT)
#error "CONFIG_MAX_PTHREAD_COUNT is insufficient to run CycloneDDS"
#endif

static int currThrIdx = 0;
K_THREAD_STACK_ARRAY_DEFINE(zephyr_stacks, CYCLONEDDS_THREAD_COUNT, CYCLONEDDS_THREAD_STACK_SIZE);

#endif

dds_return_t
ddsrt_thread_create (
  ddsrt_thread_t *thread,
  const char *name,
  const ddsrt_threadattr_t *attr,
  ddsrt_thread_routine_t start_routine,
  void *arg)
{
  pthread_attr_t pattr;
  thread_context_t *ctx;
  ddsrt_threadattr_t tattr;
  int result, create_ret;
#if !defined(__ZEPHYR__)
  sigset_t set, oset;
#endif

  assert (thread != NULL);
  assert (name != NULL);
  assert (attr != NULL);
  assert (start_routine != NULL);
  tattr = *attr;

#if defined(__ZEPHYR__)
  /* Override requested size by size of statically allocated stacks */
  if (tattr.stackSize != 0 && tattr.stackSize > CYCLONEDDS_THREAD_STACK_SIZE) {
    DDS_ERROR ("ddsrt_thread_create(%s): requested stack size (%d) exceeds maximum size (%d)\n",
      name, tattr.stackSize, CYCLONEDDS_THREAD_STACK_SIZE);
    return DDS_RETCODE_ERROR;
  } else {
    tattr.stackSize = CYCLONEDDS_THREAD_STACK_SIZE;
  }
#endif

  if (pthread_attr_init (&pattr) != 0)
    return DDS_RETCODE_ERROR;

#if defined(__VXWORKS__)
  /* pthread_setname_np is not available on VxWorks. Use pthread_attr_setname
     instead (proprietary VxWorks extension). */
  (void)pthread_attr_setname (&pattr, name);
#endif

#if !defined(__ZEPHYR__)
  if (pthread_attr_setscope (&pattr, PTHREAD_SCOPE_SYSTEM) != 0)
    goto err;
#endif

  if (pthread_attr_setdetachstate (&pattr, PTHREAD_CREATE_JOINABLE) != 0) {
    goto err;
  }

  if (tattr.stackSize != 0)
  {
#ifdef PTHREAD_STACK_MIN
    if (tattr.stackSize < (uint32_t)PTHREAD_STACK_MIN)
      tattr.stackSize = (uint32_t)PTHREAD_STACK_MIN;
#endif

#if !defined(__ZEPHYR__)
    if ((result = pthread_attr_setstacksize (&pattr, tattr.stackSize)) != 0)
    {
      DDS_ERROR ("ddsrt_thread_create(%s): pthread_attr_setstacksize(%"PRIu32") failed with error %d\n", name, tattr.stackSize, result);
      goto err;
    }
#else
    if (currThrIdx >= CYCLONEDDS_THREAD_COUNT)
    {
      DDS_ERROR ("ddsrt_thread_create(%s): CYCLONEDDS_THREAD_COUNT(%d) exceeded\n", name, currThrIdx);
      goto err;
    }
    if ((result = pthread_attr_setstack (&pattr, &zephyr_stacks[currThrIdx], tattr.stackSize)) != 0)
    {
      DDS_ERROR ("ddsrt_thread_create(%s): pthread_attr_setstack(%p, %"PRIu32") failed with error %d\n",
        name, &zephyr_stacks[currThrIdx], tattr.stackSize, result);
      goto err;
    }
    currThrIdx++;
#endif
  }

  /* For Zephyr SCHED_DEFAULT is either SCHED_RR or SCHED_FIFO, both realtime classes that
  take a priority. For other platforms, SCHED_DEFAULT with a non-default priority is rejected. */
#if !defined(__ZEPHYR__)
  if (tattr.schedClass == DDSRT_SCHED_DEFAULT)
  {
    if (tattr.schedPriority != 0)
    {
      /* If caller doesn't set the class, he must not try to set the priority, which we
       approximate by expecting a 0. FIXME: should do this as part of config validation */
      DDS_ERROR("ddsrt_thread_create(%s): schedClass DEFAULT but priority != 0 is unsupported\n", name);
      goto err;
    }
  }
  else
#endif
  {
    int policy;
    struct sched_param sched_param;
#if !defined(__ZEPHYR__)
    if ((result = pthread_getschedparam (pthread_self (), &policy, &sched_param)) != 0)
    {
      DDS_ERROR("ddsrt_thread_create(%s): pthread_getschedparam(self) failed with error %d\n", name, result);
      goto err;
    }
#else
    if ((result = pthread_attr_getschedparam(&pattr, &sched_param)) != 0)
    {
      DDS_ERROR("ddsrt_thread_create(%s): pthread_attr_getschedparam() failed with error %d\n", name, result);
      goto err;
    }
    if ((result = pthread_attr_getschedpolicy(&pattr, &policy)) != 0)
    {
      DDS_ERROR("ddsrt_thread_create(%s): pthread_attr_getschedpolicy() failed with error %d\n", name, result);
      goto err;
    }
#endif
    switch (tattr.schedClass)
    {
      case DDSRT_SCHED_DEFAULT:
#if !defined(__ZEPHYR__)
        assert (0);
#endif
        break;
      case DDSRT_SCHED_REALTIME:
        policy = SCHED_FIFO;
        break;
      case DDSRT_SCHED_TIMESHARE:
        policy = SCHED_OTHER;
        break;
    }
    if (tattr.schedClass != DDSRT_SCHED_DEFAULT)
    {
      if ((result = pthread_attr_setschedpolicy (&pattr, policy)) != 0)
      {
        DDS_ERROR("ddsrt_thread_create(%s): pthread_attr_setschedpolicy(%d) failed with error %d\n", name, policy, result);
        goto err;
      }
    }

    if ((tattr.schedPriority >= sched_get_priority_min(policy)) && (tattr.schedPriority <= sched_get_priority_max(policy)))
    {
      sched_param.sched_priority = tattr.schedPriority;
    }
    else
    {
      DDS_WARNING("ddsrt_thread_create(%s): requested thread priority(%d) invalid for policy(%d), fall-back to default\n: %d", name, tattr.schedPriority, policy, sched_param.sched_priority);
    }

    if ((result = pthread_attr_setschedparam (&pattr, &sched_param)) != 0)
    {
      DDS_ERROR("ddsrt_thread_create(%s): pthread_attr_setschedparam(priority = %d) failed with error %d\n", name, tattr.schedPriority, result);
      goto err;
    }

#if !defined(__ZEPHYR__)
    if ((result = pthread_attr_setinheritsched (&pattr, PTHREAD_EXPLICIT_SCHED)) != 0)
    {
      DDS_ERROR("ddsrt_thread_create(%s): pthread_attr_setinheritsched(EXPLICIT) failed with error %d\n", name, result);
      goto err;
    }
#endif
  }

  if (tattr.schedAffinityN > 0)
  {
#if defined (__linux) && defined(__GLIBC__)
    cpu_set_t cpuset;
    CPU_ZERO (&cpuset);
    for (uint32_t i = 0; i < tattr.schedAffinityN; i++)
    {
      if (tattr.schedAffinitySet[i] >= CPU_SETSIZE)
      {
        DDS_ERROR ("os_threadCreate(%s): CPU id %"PRIu32" out of range when setting affinity\n", name, tattr.schedAffinitySet[i]);
        goto err;
      }
      CPU_SET(tattr.schedAffinitySet[i], &cpuset);
    }
    if ((result = pthread_attr_setaffinity_np (&pattr, sizeof (cpuset), &cpuset)) != 0)
    {
      DDS_ERROR("ddsrt_thread_create(%s): pthread_attr_setinheritsched(EXPLICIT) failed with error %d\n", name, result);
      goto err;
    }
#else
    /* Didn't implement setting thread affinity for this platform yet */
    goto err;
#endif
  }

  /* Construct context structure & start thread */
  ctx = ddsrt_malloc (sizeof (thread_context_t));
  ctx->name = ddsrt_strdup(name);
  ctx->routine = start_routine;
  ctx->arg = arg;

#if !defined(__ZEPHYR__)
  /* Block signal delivery in our own threads (SIGXCPU is excluded so we have a way of
     dumping stack traces, but that should be improved upon) */
  sigfillset (&set);
#ifdef __APPLE__
  DDSRT_WARNING_GNUC_OFF(sign-conversion)
#endif
  sigdelset (&set, SIGXCPU);
#ifdef __APPLE__
  DDSRT_WARNING_GNUC_ON(sign-conversion)
#endif
  sigprocmask (SIG_BLOCK, &set, &oset);
#endif /* !defined(__ZEPHYR__) */
  if ((create_ret = pthread_create (&thread->v, &pattr, os_startRoutineWrapper, ctx)) != 0)
  {
    DDS_ERROR ("os_threadCreate(%s): pthread_create failed with error %d\n", name, create_ret);
    goto err_create;
  }
#if !defined(__ZEPHYR__)
  sigprocmask (SIG_SETMASK, &oset, NULL);
#endif
  pthread_attr_destroy (&pattr);
  return DDS_RETCODE_OK;

err_create:
  ddsrt_free (ctx->name);
  ddsrt_free (ctx);
err:
  pthread_attr_destroy (&pattr);
  return DDS_RETCODE_ERROR;
}

ddsrt_tid_t
ddsrt_gettid(void)
{
  ddsrt_tid_t tid;

#if defined(__linux)
  tid = syscall(SYS_gettid);
#elif defined(__FreeBSD__) && (__FreeBSD__ >= 9)
  /* FreeBSD >= 9.0 */
  tid = pthread_getthreadid_np();
#elif defined(__APPLE__) && !defined(__POWERPC__) && \
  !(defined(MAC_OS_X_VERSION_MIN_REQUIRED) && MAC_OS_X_VERSION_MIN_REQUIRED < 1060)
  /* macOS >= 10.6, but for ppc this symbol is unavailable */
  pthread_threadid_np(NULL, &tid);
#elif defined(__VXWORKS__)
  tid = taskIdSelf();
#else
  tid = (uintmax_t)((uintptr_t)pthread_self());
#endif

  return tid;
}

ddsrt_tid_t
ddsrt_gettid_for_thread( ddsrt_thread_t thread)
{
  return (ddsrt_tid_t) thread.v;

}


ddsrt_thread_t
ddsrt_thread_self(void)
{
  ddsrt_thread_t id = {.v = pthread_self ()};
  return id;
}

bool ddsrt_thread_equal(ddsrt_thread_t t1, ddsrt_thread_t t2)
{
  return (pthread_equal(t1.v, t2.v) != 0);
}

dds_return_t
ddsrt_thread_join(ddsrt_thread_t thread, uint32_t *thread_result)
{
  int err;
  void *vthread_result;

#if !defined(__ZEPHYR__)
/* In Zephyr, pthread_t is an array index so 0 is fine for the first pthread,
   which can be a ddsrt_thread when eg. the main thread is a native Zephyr thread */
  assert (thread.v);
#endif


  if ((err = pthread_join (thread.v, &vthread_result)) != 0)
  {
    DDS_ERROR ("pthread_join(0x%"PRIxMAX") failed with error %d\n", (uintmax_t)((uintptr_t)thread.v), err);
    return DDS_RETCODE_ERROR;
  }

  if (thread_result)
    *thread_result = (uint32_t) ((uintptr_t) vthread_result);
  return DDS_RETCODE_OK;
}

#if defined __linux
dds_return_t
ddsrt_thread_list (
  ddsrt_thread_list_id_t *tids,
  size_t size)
{
  DIR *dir;
  struct dirent *de;
  if ((dir = opendir ("/proc/self/task")) == NULL)
    return DDS_RETCODE_ERROR;
  dds_return_t n = 0;
  while ((de = readdir (dir)) != NULL)
  {
    if (de->d_name[0] == '.' && (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
      continue;
    int pos;
    long tid;
    if (sscanf (de->d_name, "%ld%n", &tid, &pos) != 1 || de->d_name[pos] != 0)
    {
      n = DDS_RETCODE_ERROR;
      break;
    }
    if ((size_t) n < size)
      tids[n] = (ddsrt_thread_list_id_t) tid;
    n++;
  }
  closedir (dir);
  /* If there were no threads, something must've gone badly wrong */
  return (n == 0) ? DDS_RETCODE_ERROR : n;
}

dds_return_t
ddsrt_thread_getname_anythread (
  ddsrt_thread_list_id_t tid,
  char *name,
  size_t size)
{
  char file[100];
  FILE *fp;
  int pos;
  pos = snprintf (file, sizeof (file), "/proc/self/task/%lu/stat", (unsigned long) tid);
  if (pos < 0 || pos >= (int) sizeof (file))
    return DDS_RETCODE_ERROR;
  if ((fp = fopen (file, "r")) == NULL)
    return DDS_RETCODE_NOT_FOUND;
  int c;
  size_t namelen = 0, namepos = 0;
  while ((c = fgetc (fp)) != EOF)
    if (c == '(')
      break;
  while ((c = fgetc (fp)) != EOF)
  {
    if (c == ')')
      namelen = namepos;
    if (namepos + 1 < size)
      name[namepos++] = (char) c;
  }
  fclose (fp);
  assert (size == 0 || namelen < size);
  if (size > 0)
    name[namelen] = 0;
  return DDS_RETCODE_OK;
}
#elif defined __QNXNTO__
dds_return_t
ddsrt_thread_list (
  ddsrt_thread_list_id_t *tids,
  size_t size)
{
  char path[32];
  int fd;
  int pos;
  procfs_info procinfo;
  int numthreads;
  pos = snprintf(path, sizeof(path), "/proc/%d/as", getpid());
  if (pos < 0 || pos >= (int)sizeof(path)) {
    return DDS_RETCODE_ERROR;
  }
  if ((fd = open(path, O_RDONLY)) == -1) {
    return DDS_RETCODE_NOT_FOUND;
  }
  memset(&procinfo, 0, sizeof(procinfo));
  if (devctl(fd, DCMD_PROC_INFO, &procinfo, sizeof(procinfo), 0) != EOK) {
    DDS_ERROR("devctl() failed for DCMD_PROC_INFO on pid %d: %s\n", 
      getpid(), strerror(errno));
    close(fd);
    return DDS_RETCODE_ERROR;
  }
  /* In QNX, thread IDs for each process start at 1 and increment sequentially,
     IDs of terminated threads are not reused */
  int tid = 1;
  int n;
  for (n=0; n < procinfo.num_threads; n++) {
    if ((size_t)n < size) {
      tids[n] = (ddsrt_thread_list_id_t)tid;
      tid++;
    } else {
      break;
    }
  }
  close(fd);
  return (n == 0) ? DDS_RETCODE_ERROR : n;
}

dds_return_t
ddsrt_thread_getname_anythread (
  ddsrt_thread_list_id_t tid,
  char *name,
  size_t size)
{
  char path[100];
  int fd;
  int pos;
  procfs_threadctl tidinfo;
  struct _thread_name *tn;
  pos = snprintf(path, sizeof(path), "/proc/%d/as", getpid());
  if (pos < 0 || pos >= (int)sizeof(path)) {
    return DDS_RETCODE_ERROR;
  }
  if ((fd = open(path, O_RDONLY)) == -1) {
    return DDS_RETCODE_NOT_FOUND;
  }
  memset(&tidinfo, 0, sizeof(tidinfo));
  tidinfo.tid = tid;
  tidinfo.cmd = _NTO_TCTL_NAME;
  tn = (struct _thread_name*)&tidinfo.data;
  tn->name_buf_len = sizeof(tidinfo.data) - sizeof(*tn);
  if (tn->name_buf_len < size) {
    close(fd);
    return DDS_RETCODE_NOT_ENOUGH_SPACE;
  }
  tn->new_name_len = -1; /* get the current name */
  if (devctl(fd, DCMD_PROC_THREADCTL, &tidinfo, sizeof(tidinfo), 0) != EOK) {
    DDS_ERROR("devctl() failed for DCMD_PROC_THREADCTL on pid %d and tid %d: %s\n",
      getpid(), tid, strerror(errno));
    close(fd);
    return DDS_RETCODE_ERROR;
  }
  if (tidinfo.tid != tid) {
    /* Requested thread has terminated, devctl() returned info for a different thread */
    close(fd);
    return DDS_RETCODE_NOT_FOUND;
  }
  strncpy(name, tn->name_buf, size - 1);
  name[size - 1] = '\0';
  close(fd);
  return DDS_RETCODE_OK;
}
#elif defined __APPLE__
DDSRT_STATIC_ASSERT (sizeof (ddsrt_thread_list_id_t) == sizeof (mach_port_t));

dds_return_t
ddsrt_thread_list (
  ddsrt_thread_list_id_t *tids,
  size_t size)
{
  thread_act_array_t tasks;
  mach_msg_type_number_t count;
  if (task_threads (mach_task_self (), &tasks, &count) != KERN_SUCCESS)
    return DDS_RETCODE_ERROR;
  for (mach_msg_type_number_t i = 0; i < count && (size_t) i < size; i++)
    tids[i] = (ddsrt_thread_list_id_t) tasks[i];
  vm_deallocate (mach_task_self (), (vm_address_t) tasks, count * sizeof (thread_act_t));
  return (dds_return_t) count;
}

dds_return_t
ddsrt_thread_getname_anythread (
  ddsrt_thread_list_id_t tid,
  char *name,
  size_t size)
{
  if (size > 0)
  {
    pthread_t pt = pthread_from_mach_thread_np ((mach_port_t) tid);
    name[0] = '\0';
    if (pt == NULL || pthread_getname_np (pt, name, size) != 0 || name[0] == 0)
      snprintf (name, size, "task%"PRIu64, (uint64_t) tid);
  }
  return DDS_RETCODE_OK;
}
#endif


static pthread_key_t thread_cleanup_key;
static pthread_once_t thread_once = PTHREAD_ONCE_INIT;

static void thread_cleanup_fini(void *arg);

static void thread_init_once(void)
{
  int err;

  err = pthread_key_create(&thread_cleanup_key, &thread_cleanup_fini);
  assert(err == 0);
  (void)err;
}

static void thread_init(void)
{
  (void)pthread_once(&thread_once, &thread_init_once);
}

dds_return_t ddsrt_thread_cleanup_push (void (*routine) (void * p), void *arg)
{
  int err;
  thread_cleanup_t *prev, *tail;

  assert(routine != NULL);

#if defined(__ZEPHYR__)
  if (pthread_self() >= CONFIG_MAX_PTHREAD_COUNT) {
    /* Not a pthread */
    return DDS_RETCODE_UNSUPPORTED;
  }
#endif

  thread_init();
  if ((tail = ddsrt_calloc(1, sizeof(*tail))) != NULL) {
    prev = pthread_getspecific(thread_cleanup_key);
    tail->prev = prev;
    tail->routine = routine;
    tail->arg = arg;
    if ((err = pthread_setspecific(thread_cleanup_key, tail)) != 0) {
      assert(err != EINVAL);
      ddsrt_free(tail); 
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    return DDS_RETCODE_OK;
  }
  return DDS_RETCODE_OUT_OF_RESOURCES;
}

dds_return_t ddsrt_thread_cleanup_pop (int execute)
{
  int err;
  thread_cleanup_t *tail;

#if defined(__ZEPHYR__)
  if (pthread_self() >= CONFIG_MAX_PTHREAD_COUNT) {
    /* Not a pthread */
    return DDS_RETCODE_UNSUPPORTED;
  }
#endif

  thread_init();
  if ((tail = pthread_getspecific(thread_cleanup_key)) != NULL) {
    if ((err = pthread_setspecific(thread_cleanup_key, tail->prev)) != 0) {
      assert(err != EINVAL);
      return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    if (execute) {
      tail->routine(tail->arg);
    }
    ddsrt_free(tail);
  }
  return DDS_RETCODE_OK;
}

static void thread_cleanup_fini(void *arg)
{
  thread_cleanup_t *tail, *prev;

  tail = (thread_cleanup_t *)arg;
  while (tail != NULL) {
    prev = tail->prev;
    assert(tail->routine != NULL);
    tail->routine(tail->arg);
    ddsrt_free(tail);
    tail = prev;
  }

  /* Thread-specific value associated with thread_cleanup_key will already be
     nullified if invoked as destructor, i.e. not from ddsrt_thread_fini. */
}

void ddsrt_thread_init(uint32_t reason)
{
  (void)reason;
  thread_init();
}

void ddsrt_thread_fini(uint32_t reason)
{
  thread_cleanup_t *tail;

  (void)reason;
  thread_init();
  if ((tail = pthread_getspecific(thread_cleanup_key)) != NULL) {
    thread_cleanup_fini(tail);
    (void)pthread_setspecific(thread_cleanup_key, NULL);
  }
}
