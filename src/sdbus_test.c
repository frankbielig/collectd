/**
 * SPDX-FileCopyrightText: 2020 Daimler AG
 **/

#include "sdbus.c"
#include "testing.h"

/* ************************************************************************** */
/* start up and shut down */
/* ************************************************************************** */

static sd_bus *bus = NULL;

static int teardown(void) {

  if (bus != NULL)
    CHECK_ZERO(sdbus_close(&bus));

  return 0;
}

static int setup(sdbus_bind_t type) {
  teardown();
  return sdbus_acquire(&bus, type);
}

/* ************************************************************************** */
/* tests */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
DEF_TEST(connect) {

  CHECK_ZERO(setup(TARGET_LOCAL_USER));
  CHECK_ZERO(setup(TARGET_LOCAL_SYSTEM));

  teardown();

  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(search) {
  if (setup(TARGET_LOCAL_USER) == 0) {
    char **names = sdbus_names(bus, false);
    int count = strv_length(names);
    INFO("found %d names", count);
    OK1(count > 0, "at least on service should be listed");
  }

  teardown();
  return 0;
}

/* -------------------------------------------------------------------------- */
DEF_TEST(read) {
  CHECK_ZERO(sdbus_init());
  OK1(sdbus_read() == 0, "read statistics");
  CHECK_ZERO(sdbus_shutdown());
  return 0;
}

/* ************************************************************************** */
/* main */
/* ************************************************************************** */

int main(void) {
  RUN_TEST(connect);
  RUN_TEST(search);
  RUN_TEST(read);

  END_TEST;
}
