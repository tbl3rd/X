#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include <tmc/cpus.h>

#include "process.h"
#include "tap.h"
#include "tilera.h"


// Define INFO(F, ...) as info(F, ## __VA_ARGS__) to enable spew.
//
// #define INFO(F, ...) info(F, ## __VA_ARGS__)
#define INFO(F, ...)


// Manage the shared process state monitor.
//
void processLock(Process *p)
{
    const int fail = pthread_mutex_lock(&p->using);
    if (fail) error("__: pthread_mutex_lock(%p) returned %d", &p->using, fail);
}
void processUnlock(Process *p)
{
    const int fail = pthread_mutex_unlock(&p->using);
    if (fail) {
        error("__: pthread_mutex_unlock(%p) returned %d", &p->using, fail);
    }
}
void processNotify(Process *p)
{
    const int fail = pthread_cond_broadcast(&p->changed);
    if (fail) {
        error("__: pthread_cond_broadcast(%p) returned %d", &p->changed, fail);
    }
}
void processWait(Process *p)
{
    const int fail = pthread_cond_wait(&p->changed, &p->using);
    if (fail) {
        error("__: pthread_cond_wait(%p, %p) returned %d",
              &p->changed, &p->using, fail);
    }
}


// Wait for all threads in p to clear their alert flags.
//
static void processWaitForThreads(Process *p, void *(*start)(void *),
                                  const char *name)
{
    INFO("__: processWaitForThreads(%p, %p, %s)", p, start, name);
    int wait = 1;
    while (wait) {
        wait = 0;
        for (int n = 0; n < p->threadCount; ++n) {
            Thread *const t = p->thread + n;
            wait = (start == t->start) && t->alert;
            if (wait) {
                INFO("__: processWaitForThreads(%p, %p, %s) waiting on %02d",
                     p, start, name, t->index);
                break;
            }
        }
        if (wait) processWait(p);
    }
}


// Stop the threads in p running start(), and return a count of the threads
// after they stop.
//
int processStopThreads(Process *p, void *(*start)(void *), const char *name)
{
    INFO("__: stopThreads(%p, %p, %s)", p, start, name);
    processLock(p);
    for (int n = 0; n < p->threadCount; ++n) {
        Thread *const t = p->thread + n;
        if (t->start == start) {
            t->alert = 1;
            INFO("__: processStopThreads(%p, %p, %s) alerted thread %02d (%p)",
                 p, start, name, t->index, t);
        }
    }
    processNotify(p);
    processWaitForThreads(p, start, name);
    processUnlock(p);
    int result = 0;
    for (int n = 0; n < p->threadCount; ++n) {
        Thread *const t = p->thread + n;
        if (t->start == start) {
            void *status;
            INFO("__: processStopThreads(%p, %p, %s) joining thread %02d (%p)",
                 p, start, name, t->index, t);
            const int fail = pthread_join(t->self, &status);
            if (fail) {
                error("__: pthread_join(%p, %p) returned %d",
                      t->self, &status, fail);
            } else {
                assert(status == (void *)t);
                ++result;
            }
        }
    }
    return result;
}


// Start all threads in p that are set up to run start().  Set pthread
// stack to the smallest permitted size.  Return the number of threads
// started after they've all started running.
//
int processStartThreads(Process *p, void *(*start)(void *), const char *name)
{
    INFO("__: processStartThreads(%p, %p, %s)", p, start, name);
    int result = 0;
    processLock(p);
    for (int n = 0; n < p->threadCount; ++n) {
        Thread *const t = p->thread + n;
        if (start == t->start) {
            t->alert = 1;
            const int fail = pthread_create(&t->self, p->attr, t->start, t);
            if (fail) {
                error("__: pthread_create(%p, %p, %p, %p) returned %d",
                      &t->self, p->attr, t->start, t, fail);
            }
            ++result;
        }
    }
    processNotify(p);
    processWaitForThreads(p, start, name);
    processUnlock(p);
    return result;
}


// Initialize some static pthread resources at p.
//
static void initializeSomePthreadStuff(Process *p)
{
    static const size_t stackSize = 131072;
    static pthread_attr_t attr;
    int fail = pthread_attr_init(&attr);
    if (fail) error("__: pthread_attr_init(%p) returned %d", &attr, fail);
    fail = pthread_attr_setstacksize(&attr, stackSize);
    if (fail) {
        error("__: pthread_attr_setstacksize(%p, %zu) returned %d",
              &attr, stackSize, fail);
    }
    p->attr = &attr;
    fail = pthread_mutex_init(&p->using, NULL);
    if (fail) {
        error("__: pthread_mutex_init(%p, NULL) returned %d",
              &p->using, fail);
    }
    fail = pthread_cond_init(&p->changed, NULL);
    if (fail) {
        error("__: pthread_cond_init(%p, NULL) returned %d",
              &p->changed, fail);
    }
}


Process *processInitialize(const char *av0, void *(*start)(void *),
                           const char *name)
{
    INFO("__: processInitialize(%s, %p, %s)", av0, start, name);
    static Process theProcess;
    static cpu_set_t cpuset;
    theProcess.av0 = av0;
    getControlIp(theProcess.control.ip);
    theProcess.control.port = CONTROLPORT;
    initializeSomePthreadStuff(&theProcess);
    tmc_cpus_get_online_cpus(&cpuset);
    theProcess.threadCount = tmc_cpus_count(&cpuset);
    assert(theProcess.threadCount <
           sizeof theProcess.thread / sizeof theProcess.thread[0]);
    for (int n = 0; n < theProcess.threadCount; ++n) {
        Thread *const t = theProcess.thread + n;
        t->index = n;
        t->cpu = tmc_cpus_find_nth_cpu(&cpuset, t->index);
        t->start = start;               // Overwrite 2 of these below.
        t->process = &theProcess;
    }
    Thread *const tMain = theProcess.thread + 0;
    Thread *const tTap = theProcess.thread + 1;
    tMain->start = NULL;                // For main().
    tMain->self = pthread_self();
    const int fail = tmc_cpus_set_my_cpu(tMain->cpu);
    if (fail) {
        error("__: tmc_cpus_set_my_cpu(%d) returned %d", tMain->cpu, fail);
    }
    tTap->start = tapStart;
    theProcess.netioThreadIndex = 2;
    theProcess.netioThreadCount = theProcess.threadCount - 2;
    return &theProcess;
}


void processUninitialize(Process *p)
{
    INFO("__: processUninitialize(%p)", p);
    const int fail = pthread_attr_destroy(p->attr);
    if (fail) {
        error("__: pthread_attr_destroy(%p) returned %d", p->attr, fail);
    } else {
        p->attr = NULL;
    }
}
