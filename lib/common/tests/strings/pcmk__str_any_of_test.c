/*
 * Copyright 2020-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

static void
empty_input_list(void **state) {
    assert_false(pcmk__strcase_any_of("xxx", NULL));
    assert_false(pcmk__str_any_of("xxx", NULL));
    assert_false(pcmk__strcase_any_of("", NULL));
    assert_false(pcmk__str_any_of("", NULL));
}

static void
empty_string(void **state) {
    assert_false(pcmk__strcase_any_of("", "xxx", "yyy", NULL));
    assert_false(pcmk__str_any_of("", "xxx", "yyy", NULL));
    assert_false(pcmk__strcase_any_of(NULL, "xxx", "yyy", NULL));
    assert_false(pcmk__str_any_of(NULL, "xxx", "yyy", NULL));
}

static void
in_list(void **state) {
    assert_true(pcmk__strcase_any_of("xxx", "aaa", "bbb", "xxx", NULL));
    assert_true(pcmk__str_any_of("xxx", "aaa", "bbb", "xxx", NULL));
    assert_true(pcmk__strcase_any_of("XXX", "aaa", "bbb", "xxx", NULL));
}

static void
not_in_list(void **state) {
    assert_false(pcmk__strcase_any_of("xxx", "aaa", "bbb", NULL));
    assert_false(pcmk__str_any_of("xxx", "aaa", "bbb", NULL));
    assert_false(pcmk__str_any_of("AAA", "aaa", "bbb", NULL));
}

int main(int argc, char **argv)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(empty_input_list),
        cmocka_unit_test(empty_string),
        cmocka_unit_test(in_list),
        cmocka_unit_test(not_in_list),
    };

    cmocka_set_message_output(CM_OUTPUT_TAP);
    return cmocka_run_group_tests(tests, NULL, NULL);
}
