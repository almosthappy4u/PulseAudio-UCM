/***
  This file is part of PulseAudio.

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

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include <pulse/util.h>
#include <pulse/xmalloc.h>
#include <pulsecore/asyncq.h>
#include <pulsecore/thread.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

static void producer(void *_q) {
    pa_asyncq *q = _q;
    int i;

    for (i = 0; i < 1000; i++) {
        printf("pushing %i\n", i);
        pa_asyncq_push(q, PA_UINT_TO_PTR(i+1), 1);
    }

    pa_asyncq_push(q, PA_UINT_TO_PTR(-1), TRUE);
    printf("pushed end\n");
}

static void consumer(void *_q) {
    pa_asyncq *q = _q;
    void *p;
    int i;

    sleep(1);

    for (i = 0;; i++) {
        p = pa_asyncq_pop(q, TRUE);

        if (p == PA_UINT_TO_PTR(-1))
            break;

        pa_assert(p == PA_UINT_TO_PTR(i+1));

        printf("popped %i\n", i);
    }

    printf("popped end\n");
}

int main(int argc, char *argv[]) {
    pa_asyncq *q;
    pa_thread *t1, *t2;

    pa_assert_se(q = pa_asyncq_new(0));

    pa_assert_se(t1 = pa_thread_new(producer, q));
    pa_assert_se(t2 = pa_thread_new(consumer, q));

    pa_thread_free(t1);
    pa_thread_free(t2);

    pa_asyncq_free(q, NULL);

    return 0;
}
