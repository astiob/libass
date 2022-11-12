/*
 * Copyright (C) 2015 Oleg Oshmyan <chortos@inbox.lv>
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

#ifndef LIBASS_COMPAT_H
#define LIBASS_COMPAT_H

#include "config.h"

#include <stdbool.h>
#include <stdlib.h>

#ifdef _MSC_VER
#define _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_SECURE_NO_WARNINGS
#define _USE_MATH_DEFINES
#define inline __inline
#endif

// Work around build failures on Windows with static FriBidi.
// Possible because we only use FriBidi functions, not objects.
#if defined(_WIN32) && !defined(FRIBIDI_LIB_STATIC)
#define FRIBIDI_LIB_STATIC
#endif

#ifndef HAVE_STRDUP
char *ass_strdup_fallback(const char *s); // definition in ass_utils.c
#define strdup ass_strdup_fallback
#endif

#ifndef HAVE_STRNDUP
#include <stddef.h>
char *ass_strndup_fallback(const char *s, size_t n); // definition in ass_utils.c
#define strndup ass_strndup_fallback
#endif

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <libloaderapi.h>
#define ENABLE_THREADS 1

typedef LONG ASSAtomicInt;

#define assi_inc_int InterlockedIncrement
#define assi_dec_int InterlockedDecrement

#ifdef _WIN64
typedef LONG64 ASSAtomicSize;

#define assi_inc_size InterlockedIncrement64
#define assi_dec_size InterlockedDecrement64

#else
typedef LONG ASSAtomicSize;

#define assi_inc_size InterlockedIncrement
#define assi_dec_size InterlockedDecrement
#endif

typedef CRITICAL_SECTION   ASSMutex;
typedef CONDITION_VARIABLE ASSCond;
typedef HANDLE             ASSThread;
typedef DWORD              ASSThreadReturn;

#define ASS_THREAD_FUNC_ATTR WINAPI

static inline bool assi_mutex_init(LPCRITICAL_SECTION mtx, bool recursive)
{
    (void)recursive; // Critical sections always allow recursive access
    InitializeCriticalSection(mtx);
    return true;
}

#define assi_mutex_destroy DeleteCriticalSection
#define assi_mutex_lock EnterCriticalSection
#define assi_mutex_unlock LeaveCriticalSection

static inline bool assi_cond_init(PCONDITION_VARIABLE cond)
{
    InitializeConditionVariable(cond);
    return true;
}

#define assi_cond_destroy(x) ((void)0) // No-op
#define assi_cond_signal WakeConditionVariable
#define assi_cond_broadcast WakeAllConditionVariable

static inline void assi_cond_wait(PCONDITION_VARIABLE cond, PCRITICAL_SECTION mtx)
{
    SleepConditionVariableCS(cond, mtx, INFINITE);
}

static inline bool assi_thread_create(HANDLE *thread, LPTHREAD_START_ROUTINE func, void *arg)
{
    *thread = CreateThread(NULL, 0, func, arg, 0, NULL);
    return *thread != NULL;
}

static inline bool assi_thread_join(HANDLE thread)
{
    bool ret = (WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
    return ret;
}

static inline void assi_set_thread_namew(PCWSTR name)
{
#if !(defined(WINAPI_FAMILY) && (WINAPI_FAMILY != WINAPI_FAMILY_DESKTOP_APP))
    HMODULE dll = GetModuleHandleW(L"kernel32.dll");
    if (!dll)
        return;

    HRESULT (WINAPI *func)(HANDLE, PCWSTR) = (void*)GetProcAddress(dll, "SetThreadDescription");
    if (!func)
        return;

    func(GetCurrentThread(), name);
#endif
}
#define assi_set_thread_name(x) assi_set_thread_namew(L"" x)

#elif CONFIG_PTHREAD

#include <stdatomic.h>
#include <pthread.h>
#define ENABLE_THREADS 1

typedef atomic_int    ASSAtomicInt;
typedef atomic_size_t ASSAtomicSize;

#define assi_inc_int(x) ++(*x)
#define assi_dec_int(x) --(*x)

#define assi_inc_size(x) ++(*x)
#define assi_dec_size(x) --(*x)

typedef pthread_mutex_t ASSMutex;
typedef pthread_cond_t  ASSCond;
typedef pthread_t       ASSThread;
typedef void*           ASSThreadReturn;

#define ASS_THREAD_FUNC_ATTR

static inline bool assi_mutex_init(pthread_mutex_t *mtx, bool recursive)
{
    bool ret = false;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return false;

    if (recursive && (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0))
        goto faila;

    if (pthread_mutex_init(mtx, &attr) == 0)
        ret = true;

faila:
    pthread_mutexattr_destroy(&attr);
    return ret;
}

#define assi_mutex_destroy pthread_mutex_destroy
#define assi_mutex_lock pthread_mutex_lock
#define assi_mutex_unlock pthread_mutex_unlock

static inline bool assi_cond_init(pthread_cond_t *cond)
{
    return pthread_cond_init(cond, NULL) == 0;
}

#define assi_cond_destroy pthread_cond_destroy
#define assi_cond_signal pthread_cond_signal
#define assi_cond_broadcast pthread_cond_broadcast
#define assi_cond_wait pthread_cond_wait

static inline bool assi_thread_create(pthread_t *thread, void *(*func)(void*), void *arg)
{
    return pthread_create(thread, NULL, func, arg) == 0;
}

static inline bool assi_thread_join(pthread_t thread)
{
    return pthread_join(thread, NULL) == 0;
}

static inline void assi_set_thread_name(const char* name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__FreeBSD__)
    pthread_set_name_np(pthread_self(), name);
#endif
}

#else

#define ENABLE_THREADS 0

typedef int    ASSAtomicInt;
typedef size_t ASSAtomicSize;

#define assi_inc_int(x) ++(*x)
#define assi_dec_int(x) --(*x)

#define assi_inc_size(x) ++(*x)
#define assi_dec_size(x) --(*x)

#endif

#endif                          /* LIBASS_COMPAT_H */
