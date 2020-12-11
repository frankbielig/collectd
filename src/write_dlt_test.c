/**
 * SPDX-FileCopyrightText: 2020 Daimler AG
 **/


#include "testing.h"
#include "write_dlt.c" 

/* ========================================================================== */
/* start up and shut down */
/* ========================================================================== */

static int setup(void) {
  int status;
  status = wdlt_init();
  if (status != 0) {
    printf("ERROR: wdlt_init() failed with %d\n", status);
    return -1;
  }

  return 0;
}

static int teardown(void) {
  int status;
  status = wdlt_shutdown();
  if (status != 0) {
    printf("error: wdlt_shutdown() failed with %d\n", status);
    return -1;
  }

  return 0;
}

/* ========================================================================== */
/* tests */
/* ========================================================================== */

/* -------------------------------------------------------------------------- */
DEF_TEST(level_matching) {
  DltLogLevelType level;
  if (setup() == 0) {
    level = wdlt_level_list_get("abc");
    EXPECT_EQ_INT(DLT_LOG_INFO, level);
  }
  teardown();

  return 0;
}

/* ========================================================================== */
/* main */
/* ========================================================================== */

int main(void) {
  RUN_TEST(level_matching);

  END_TEST;
}
