/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pthread.h>
#include <sched.h>
#include <errno.h>

#include <pulse/xmalloc.h>
#include <pulsecore/mutex.h>
#include <pulsecore/once.h>
#include <pulsecore/atomic.h>
#include <pulsecore/macro.h>

#include "thread.h"

struct pa_thread {
    pthread_t id;
    pa_thread_func_t thread_func;
    void *userdata;
    pa_atomic_t running;
    pa_bool_t joined;
};

struct pa_tls {
    pthread_key_t key;
};

static void thread_free_cb(void *p) {
    pa_thread *t = p;

    pa_assert(t);

    if (!t->thread_func)
        /* This is a foreign thread, we need to free the struct */
        pa_xfree(t);
}

PA_STATIC_TLS_DECLARE(current_thread, thread_free_cb);

static void* internal_thread_func(void *userdata) {
    pa_thread *t = userdata;
    pa_assert(t);

    t->id = pthread_self();

    PA_STATIC_TLS_SET(current_thread, t);

    pa_atomic_inc(&t->running);
    t->thread_func(t->userdata);
    pa_atomic_sub(&t->running, 2);

    return NULL;
}

pa_thread* pa_thread_new(pa_thread_func_t thread_func, void *userdata) {
    pa_thread *t;

    pa_assert(thread_func);

    t = pa_xnew0(pa_thread, 1);
    t->thread_func = thread_func;
    t->userdata = userdata;

    if (pthread_create(&t->id, NULL, internal_thread_func, t) < 0) {
        pa_xfree(t);
        return NULL;
    }

    pa_atomic_inc(&t->running);

    return t;
}

int pa_thread_is_running(pa_thread *t) {
    pa_assert(t);

    /* Unfortunately there is no way to tell whether a "foreign"
     * thread is still running. See
     * http://udrepper.livejournal.com/16844.html for more
     * information */
    pa_assert(t->thread_func);

    return pa_atomic_load(&t->running) > 0;
}

void pa_thread_free(pa_thread *t) {
    pa_assert(t);

    pa_thread_join(t);
    pa_xfree(t);
}

int pa_thread_join(pa_thread *t) {
    pa_assert(t);
    pa_assert(t->thread_func);

    if (t->joined)
        return -1;

    t->joined = TRUE;
    return pthread_join(t->id, NULL);
}

pa_thread* pa_thread_self(void) {
    pa_thread *t;

    if ((t = PA_STATIC_TLS_GET(current_thread)))
        return t;

    /* This is a foreign thread, let's create a pthread structure to
     * make sure that we can always return a sensible pointer */

    t = pa_xnew0(pa_thread, 1);
    t->id = pthread_self();
    t->joined = TRUE;
    pa_atomic_store(&t->running, 2);

    PA_STATIC_TLS_SET(current_thread, t);

    return t;
}

void* pa_thread_get_data(pa_thread *t) {
    pa_assert(t);

    return t->userdata;
}

void pa_thread_set_data(pa_thread *t, void *userdata) {
    pa_assert(t);

    t->userdata = userdata;
}

void pa_thread_yield(void) {
#ifdef HAVE_PTHREAD_YIELD
    pthread_yield();
#else
    pa_assert_se(sched_yield() == 0);
#endif
}

pa_tls* pa_tls_new(pa_free_cb_t free_cb) {
    pa_tls *t;

    t = pa_xnew(pa_tls, 1);

    if (pthread_key_create(&t->key, free_cb) < 0) {
        pa_xfree(t);
        return NULL;
    }

    return t;
}

void pa_tls_free(pa_tls *t) {
    pa_assert(t);

    pa_assert_se(pthread_key_delete(t->key) == 0);
    pa_xfree(t);
}

void *pa_tls_get(pa_tls *t) {
    pa_assert(t);

    return pthread_getspecific(t->key);
}

void *pa_tls_set(pa_tls *t, void *userdata) {
    void *r;

    r = pthread_getspecific(t->key);
    pa_assert_se(pthread_setspecific(t->key, userdata) == 0);
    return r;
}
