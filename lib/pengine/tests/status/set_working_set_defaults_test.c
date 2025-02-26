/*
 * Copyright 2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>

#include <crm/common/scheduler.h>
#include <crm/pengine/internal.h>
#include <crm/pengine/status.h>

#include "mock_private.h"

static void
check_defaults(void **state) {
    uint32_t flags;
    pcmk_scheduler_t *scheduler = calloc(1, sizeof(pcmk_scheduler_t));

    set_working_set_defaults(scheduler);

    flags = pcmk_sched_symmetric_cluster
            |pcmk_sched_stop_removed_resources
            |pcmk_sched_cancel_removed_actions;

    if (!strcmp(PCMK__CONCURRENT_FENCING_DEFAULT, "true")) {
        flags |= pcmk_sched_concurrent_fencing;
    }


    assert_null(scheduler->priv);
    assert_int_equal(scheduler->order_id, 1);
    assert_int_equal(scheduler->action_id, 1);
    assert_int_equal(scheduler->no_quorum_policy, pcmk_no_quorum_stop);
    assert_int_equal(scheduler->flags, flags);

    /* Avoid calling pe_free_working_set here so we don't artificially
     * inflate the coverage numbers.
     */
    free(scheduler);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(check_defaults))
