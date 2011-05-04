/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>
#include <pulse/rtclock.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>

#include "module-suspend-on-idle-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("When a sink/source is idle for too long, suspend it");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    "timeout",
    NULL,
};

struct userdata {
    pa_core *core;
    pa_usec_t timeout;
    pa_hashmap *device_infos;
    pa_hook_slot
        *sink_new_slot,
        *source_new_slot,
        *sink_unlink_slot,
        *source_unlink_slot,
        *sink_state_changed_slot,
        *source_state_changed_slot;

    pa_hook_slot
        *sink_input_new_slot,
        *source_output_new_slot,
        *sink_input_unlink_slot,
        *source_output_unlink_slot,
        *sink_input_move_start_slot,
        *source_output_move_start_slot,
        *sink_input_move_finish_slot,
        *source_output_move_finish_slot,
        *sink_input_state_changed_slot,
        *source_output_state_changed_slot;
};

struct device_info {
    struct userdata *userdata;
    pa_sink *sink;
    pa_source *source;
    pa_usec_t last_use;
    pa_time_event *time_event;
};

static void timeout_cb(pa_mainloop_api*a, pa_time_event* e, const struct timeval *t, void *userdata) {
    struct device_info *d = userdata;

    pa_assert(d);

    d->userdata->core->mainloop->time_restart(d->time_event, NULL);

    if (d->sink && pa_sink_check_suspend(d->sink) <= 0 && !(d->sink->suspend_cause & PA_SUSPEND_IDLE)) {
        pa_log_info("Sink %s idle for too long, suspending ...", d->sink->name);
        pa_sink_suspend(d->sink, TRUE, PA_SUSPEND_IDLE);
    }

    if (d->source && pa_source_check_suspend(d->source) <= 0 && !(d->source->suspend_cause & PA_SUSPEND_IDLE)) {
        pa_log_info("Source %s idle for too long, suspending ...", d->source->name);
        pa_source_suspend(d->source, TRUE, PA_SUSPEND_IDLE);
    }
}

static void restart(struct device_info *d) {
    pa_usec_t now;
    const char *s;
    uint32_t timeout;

    pa_assert(d);
    pa_assert(d->sink || d->source);

    d->last_use = now = pa_rtclock_now();

    s = pa_proplist_gets(d->sink ? d->sink->proplist : d->source->proplist, "module-suspend-on-idle.timeout");
    if (!s || pa_atou(s, &timeout) < 0)
        timeout = d->userdata->timeout;

    pa_core_rttime_restart(d->userdata->core, d->time_event, now + timeout * PA_USEC_PER_SEC);

    if (d->sink)
        pa_log_debug("Sink %s becomes idle, timeout in %u seconds.", d->sink->name, timeout);
    if (d->source)
        pa_log_debug("Source %s becomes idle, timeout in %u seconds.", d->source->name, timeout);
}

static void resume(struct device_info *d) {
    pa_assert(d);

    d->userdata->core->mainloop->time_restart(d->time_event, NULL);

    if (d->sink) {
        pa_sink_suspend(d->sink, FALSE, PA_SUSPEND_IDLE);

        pa_log_debug("Sink %s becomes busy.", d->sink->name);
    }

    if (d->source) {
        pa_source_suspend(d->source, FALSE, PA_SUSPEND_IDLE);

        pa_log_debug("Source %s becomes busy.", d->source->name);
    }
}

static pa_hook_result_t sink_input_fixate_hook_cb(pa_core *c, pa_sink_input_new_data *data, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    if (data->flags & PA_SINK_INPUT_START_CORKED)
        return PA_HOOK_OK;

    if ((d = pa_hashmap_get(u->device_infos, data->sink)))
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_fixate_hook_cb(pa_core *c, pa_source_output_new_data *data, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    if (data->flags & PA_SOURCE_OUTPUT_START_CORKED)
        return PA_HOOK_OK;

    if (data->source->monitor_of)
        d = pa_hashmap_get(u->device_infos, data->source->monitor_of);
    else
        d = pa_hashmap_get(u->device_infos, data->source);

    if (d)
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    if (!s->sink)
        return PA_HOOK_OK;

    if (pa_sink_check_suspend(s->sink) <= 0) {
        struct device_info *d;
        if ((d = pa_hashmap_get(u->device_infos, s->sink)))
            restart(d);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_unlink_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    struct device_info *d = NULL;

    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (!s->source)
        return PA_HOOK_OK;

    if (s->source->monitor_of) {
        if (pa_sink_check_suspend(s->source->monitor_of) <= 0)
            d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
    } else {
        if (pa_source_check_suspend(s->source) <= 0)
            d = pa_hashmap_get(u->device_infos, s->source);
    }

    if (d)
        restart(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_start_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    if (pa_sink_check_suspend(s->sink) <= 1)
        if ((d = pa_hashmap_get(u->device_infos, s->sink)))
            restart(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_finish_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    struct device_info *d;
    pa_sink_input_state_t state;

    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    state = pa_sink_input_get_state(s);
    if (state != PA_SINK_INPUT_RUNNING && state != PA_SINK_INPUT_DRAINED)
        return PA_HOOK_OK;

    if ((d = pa_hashmap_get(u->device_infos, s->sink)))
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_move_start_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    struct device_info *d = NULL;

    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (s->source->monitor_of) {
        if (pa_sink_check_suspend(s->source->monitor_of) <= 1)
            d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
    } else {
        if (pa_source_check_suspend(s->source) <= 1)
            d = pa_hashmap_get(u->device_infos, s->source);
    }

    if (d)
        restart(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_move_finish_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (pa_source_output_get_state(s) != PA_SOURCE_OUTPUT_RUNNING)
        return PA_HOOK_OK;

    if (s->source->monitor_of)
        d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
    else
        d = pa_hashmap_get(u->device_infos, s->source);

    if (d)
        resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_state_changed_hook_cb(pa_core *c, pa_sink_input *s, struct userdata *u) {
    struct device_info *d;
    pa_sink_input_state_t state;

    pa_assert(c);
    pa_sink_input_assert_ref(s);
    pa_assert(u);

    state = pa_sink_input_get_state(s);
    if (state == PA_SINK_INPUT_RUNNING || state == PA_SINK_INPUT_DRAINED)
        if ((d = pa_hashmap_get(u->device_infos, s->sink)))
            resume(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_state_changed_hook_cb(pa_core *c, pa_source_output *s, struct userdata *u) {
    pa_assert(c);
    pa_source_output_assert_ref(s);
    pa_assert(u);

    if (pa_source_output_get_state(s) == PA_SOURCE_OUTPUT_RUNNING) {
        struct device_info *d;

        if (s->source->monitor_of)
            d = pa_hashmap_get(u->device_infos, s->source->monitor_of);
        else
            d = pa_hashmap_get(u->device_infos, s->source);

        if (d)
            resume(d);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t device_new_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct device_info *d;
    pa_source *source;
    pa_sink *sink;

    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    source = pa_source_isinstance(o) ? PA_SOURCE(o) : NULL;
    sink = pa_sink_isinstance(o) ? PA_SINK(o) : NULL;

    /* Never suspend monitors */
    if (source && source->monitor_of)
        return PA_HOOK_OK;

    pa_assert(source || sink);

    d = pa_xnew(struct device_info, 1);
    d->userdata = u;
    d->source = source ? pa_source_ref(source) : NULL;
    d->sink = sink ? pa_sink_ref(sink) : NULL;
    d->time_event = pa_core_rttime_new(c, PA_USEC_INVALID, timeout_cb, d);
    pa_hashmap_put(u->device_infos, o, d);

    if ((d->sink && pa_sink_check_suspend(d->sink) <= 0) ||
        (d->source && pa_source_check_suspend(d->source) <= 0))
        restart(d);

    return PA_HOOK_OK;
}

static void device_info_free(struct device_info *d) {
    pa_assert(d);

    if (d->source)
        pa_source_unref(d->source);
    if (d->sink)
        pa_sink_unref(d->sink);

    d->userdata->core->mainloop->time_free(d->time_event);

    pa_xfree(d);
}

static pa_hook_result_t device_unlink_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    if ((d = pa_hashmap_remove(u->device_infos, o)))
        device_info_free(d);

    return PA_HOOK_OK;
}

static pa_hook_result_t device_state_changed_hook_cb(pa_core *c, pa_object *o, struct userdata *u) {
    struct device_info *d;

    pa_assert(c);
    pa_object_assert_ref(o);
    pa_assert(u);

    if (!(d = pa_hashmap_get(u->device_infos, o)))
        return PA_HOOK_OK;

    if (pa_sink_isinstance(o)) {
        pa_sink *s = PA_SINK(o);
        pa_sink_state_t state = pa_sink_get_state(s);

        if (pa_sink_check_suspend(s) <= 0)
            if (PA_SINK_IS_OPENED(state))
                restart(d);

    } else if (pa_source_isinstance(o)) {
        pa_source *s = PA_SOURCE(o);
        pa_source_state_t state = pa_source_get_state(s);

        if (pa_source_check_suspend(s) <= 0)
            if (PA_SOURCE_IS_OPENED(state))
                restart(d);
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    uint32_t timeout = 5;
    uint32_t idx;
    pa_sink *sink;
    pa_source *source;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "timeout", &timeout) < 0) {
        pa_log("Failed to parse timeout value.");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->core = m->core;
    u->timeout = timeout;
    u->device_infos = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    for (sink = pa_idxset_first(m->core->sinks, &idx); sink; sink = pa_idxset_next(m->core->sinks, &idx))
        device_new_hook_cb(m->core, PA_OBJECT(sink), u);

    for (source = pa_idxset_first(m->core->sources, &idx); source; source = pa_idxset_next(m->core->sources, &idx))
        device_new_hook_cb(m->core, PA_OBJECT(source), u);

    u->sink_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_NORMAL, (pa_hook_cb_t) device_new_hook_cb, u);
    u->source_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_PUT], PA_HOOK_NORMAL, (pa_hook_cb_t) device_new_hook_cb, u);
    u->sink_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) device_unlink_hook_cb, u);
    u->source_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) device_unlink_hook_cb, u);
    u->sink_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) device_state_changed_hook_cb, u);
    u->source_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) device_state_changed_hook_cb, u);

    u->sink_input_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_fixate_hook_cb, u);
    u->source_output_new_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_fixate_hook_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_unlink_hook_cb, u);
    u->source_output_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_unlink_hook_cb, u);
    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_move_start_hook_cb, u);
    u->source_output_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_START], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_move_start_hook_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_move_finish_hook_cb, u);
    u->source_output_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FINISH], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_move_finish_hook_cb, u);
    u->sink_input_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_input_state_changed_hook_cb, u);
    u->source_output_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) source_output_state_changed_hook_cb, u);

    pa_modargs_free(ma);
    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    struct device_info *d;

    pa_assert(m);

    if (!m->userdata)
        return;

    u = m->userdata;

    if (u->sink_new_slot)
        pa_hook_slot_free(u->sink_new_slot);
    if (u->sink_unlink_slot)
        pa_hook_slot_free(u->sink_unlink_slot);
    if (u->sink_state_changed_slot)
        pa_hook_slot_free(u->sink_state_changed_slot);

    if (u->source_new_slot)
        pa_hook_slot_free(u->source_new_slot);
    if (u->source_unlink_slot)
        pa_hook_slot_free(u->source_unlink_slot);
    if (u->source_state_changed_slot)
        pa_hook_slot_free(u->source_state_changed_slot);

    if (u->sink_input_new_slot)
        pa_hook_slot_free(u->sink_input_new_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_input_move_start_slot)
        pa_hook_slot_free(u->sink_input_move_start_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);

    if (u->source_output_new_slot)
        pa_hook_slot_free(u->source_output_new_slot);
    if (u->source_output_unlink_slot)
        pa_hook_slot_free(u->source_output_unlink_slot);
    if (u->source_output_move_start_slot)
        pa_hook_slot_free(u->source_output_move_start_slot);
    if (u->source_output_move_finish_slot)
        pa_hook_slot_free(u->source_output_move_finish_slot);
    if (u->source_output_state_changed_slot)
        pa_hook_slot_free(u->source_output_state_changed_slot);

    while ((d = pa_hashmap_steal_first(u->device_infos)))
        device_info_free(d);

    pa_hashmap_free(u->device_infos, NULL, NULL);

    pa_xfree(u);
}
