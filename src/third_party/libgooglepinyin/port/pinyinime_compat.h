/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Patch P2/P4 (lvgl_pinyin_ime): portability shim force-included into the
 * vendored libgooglepinyin translation units.
 *
 * Upstream is written for Android/Bionic and glibc. Two things do not exist on
 * every toolchain we target:
 *
 *   - <pthread.h> with PTHREAD_MUTEX_INITIALIZER / pthread_mutex_trylock().
 *     ESP-IDF and glibc have them, MinGW (the host unit-test toolchain) does
 *     not. The shim substitutes a std::recursive_mutex with the same call
 *     surface.
 *   - <sys/time.h> gettimeofday(); available on ESP-IDF (newlib) and mingw, so
 *     it is left alone.
 *
 * The shim is included with -include, never edited into the vendored sources,
 * so diffing against upstream stays trivial.
 */

#pragma once

/* Feature detection first: ESP-IDF and glibc provide pthread, MinGW does not.
 * Using __has_include here (rather than assuming the host) keeps the shim
 * working on the target too. */
#if defined(__has_include)
#if __has_include(<pthread.h>)
#define IME_PORT_HAVE_PTHREAD 1
#endif
#endif

#if defined(IME_PORT_HAVE_PTHREAD)
#include <pthread.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_log.h>
#endif
#include <sys/time.h>
#else

#include <cstdio>
#include <mutex>

typedef std::recursive_mutex ime_port_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER
#define pthread_mutex_t ime_port_mutex_t

/* Upstream only ever passes the single global mutex, which has static storage
 * duration: constructing it on first use is enough. */
namespace ime_port {
inline std::recursive_mutex &static_mutex()
{
    static std::recursive_mutex m;
    return m;
}
} // namespace ime_port

#define pthread_mutex_lock(m) ime_port::static_mutex().lock()
#define pthread_mutex_unlock(m) ime_port::static_mutex().unlock()
#define pthread_mutex_trylock(m) (ime_port::static_mutex().try_lock() ? 0 : 1)

#endif /* IME_PORT_HAVE_PTHREAD */

/* Patch P6: upstream leaves debug printf() calls in the runtime sources. They
 * are all guarded by the const bool kPrintDebugX flags, but keep the door open
 * for a quiet build by routing diagnostics through one macro. */
#ifndef IME_PORT_TRACE
#define IME_PORT_TRACE(...) ((void)0)
#endif
