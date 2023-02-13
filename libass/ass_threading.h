/*
 * Copyright (C) 2023 libass contributors
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef LIBASS_THREADING_H
#define LIBASS_THREADING_H

#include "config.h"

#include "ass_compat.h"

#include <stdbool.h>
#include <stdlib.h>

#if HAVE_STDATOMIC_H

#include <stdatomic.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define ENABLE_THREADS 1

struct ThreadStruct {
    void *(*start_routine)(void*);
    void *arg;
    void *ret;
    HANDLE handle;
};

typedef struct ThreadStruct *pthread_t;

typedef CRITICAL_SECTION   pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

static inline int pthread_mutex_init(pthread_mutex_t *mtx, const void *attr)
{
    InitializeCriticalSection(mtx);
    assert(!attr);
    return 0;
}

#define pthread_mutex_destroy DeleteCriticalSection
#define pthread_mutex_lock EnterCriticalSection
#define pthread_mutex_unlock LeaveCriticalSection

static inline bool pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    assert(!attr);
    InitializeConditionVariable(cond);
    return 0;
}

#define pthread_cond_destroy(x) ((void)0) // No-op
#define pthread_cond_signal WakeConditionVariable
#define pthread_cond_broadcast WakeAllConditionVariable

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return (SleepConditionVariableCS(cond, mutex, INFINITE) == 0) ? 0 : EINVAL;
}

static DWORD WINAPI
#ifdef __GNUC__
__attribute__((force_align_arg_pointer))
#endif
thread_start_func(void *arg)
{
    pthread_t thread = arg;
    thread->ret = thread->start_routine(thread->arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void*), void *arg)
{
    *thread = malloc(sizeof(struct ThreadStruct));
    assert(!attr);
    (*thread)->start_routine = start_routine;
    (*thread)->arg = arg;
    (*thread)->handle = CreateThread(NULL, 0, thread_start_func, *thread, 0, NULL);
    return (*thread)->handle == NULL ? EINVAL : 0;
}

static inline int pthread_join(pthread_t thread, void **value_ptr)
{
    DWORD ret = WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);

    if (ret == WAIT_OBJECT_0 && value_ptr)
        *value_ptr = thread->ret;

    free(thread);

    return ret == WAIT_OBJECT_0 ? 0 : EINVAL;
}

static inline void thread_set_namew(PCWSTR name)
{
#if ASS_WINAPI_DESKTOP
    HMODULE dll = GetModuleHandleW(L"kernel32.dll");
    if (!dll)
        return;

    HRESULT (WINAPI *func)(HANDLE, PCWSTR) = (void*)GetProcAddress(dll, "SetThreadDescription");
    if (!func)
        return;

    func(GetCurrentThread(), name);
#endif
}
#define thread_set_name(x) thread_set_namew(L"" x)

#elif CONFIG_PTHREAD

#include <pthread.h>
#define ENABLE_THREADS 1

static inline void thread_set_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__FreeBSD__)
    pthread_set_name_np(pthread_self(), name);
#endif
}

#endif /* CONFIG_PTHREAD */

#endif /* HAVE_STDATOMIC_H */

#ifndef ASS_THREAD_FUNC_ATTR
#define ASS_THREAD_FUNC_ATTR
#endif

#ifdef ENABLE_THREADS

#define inc_ref(x) (void)(atomic_fetch_add_explicit(x, 1, memory_order_relaxed))
#define dec_ref(x) (atomic_fetch_sub_explicit(x, 1, memory_order_acq_rel) - 1)

static inline unsigned default_threads(void)
{
    if (getenv("LIBASS_NO_THREADS"))
        return 1;

#ifdef _SC_NPROCESSORS_ONLN
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    if (sc < 0)
        return 1;
    return sc;
#elif defined(_WIN32)
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return info.dwNumberOfProcessors;
#endif

    return 1;
}

#else

#define ENABLE_THREADS 0

static inline void inc_ref(size_t *count) { ++(*count); }
static inline size_t dec_ref(size_t *count) { return --(*count); }

#ifndef _Atomic
#define _Atomic
#endif

#define pthread_mutex_lock(x) do {} while(0)
#define pthread_mutex_unlock(x) do {} while(0)
#define pthread_cond_signal(x) do {} while(0)
#define pthread_cond_broadcast(x) do {} while(0)

#endif /* !ENABLE_THREADS */

#endif                          /* LIBASS_THREADING_H */
