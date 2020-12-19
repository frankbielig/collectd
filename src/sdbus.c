/**
 * SPDX-FileCopyrightText: 2020 Daimler AG
 **/

#include "plugin.h"
#include "utils/common/common.h"
#include "utils_time.h"
#include "collectd.h"

#include "systemd/sd-bus.h"

/* ************************************************************************** */
/* types */
/* ************************************************************************** */

typedef enum {
  TARGET_LOCAL,
  TARGET_LOCAL_USER,
  TARGET_LOCAL_SYSTEM
} sdbus_bind_t;

/* ************************************************************************** */
/* constants */
/* ************************************************************************** */

#define LOG_KEY "sdbus: "
#define LOG_KEY_NAMES LOG_KEY "sdbus_names - "

/* ************************************************************************** */
/* global variables */
/* ************************************************************************** */

static sd_bus *bus_user = NULL;
static sd_bus *bus_system = NULL;

/* ************************************************************************** */
/* helper functions */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
char **strv_free(char **strv) {
  char **str;

  if (!strv)
    return NULL;

  for (str = strv; *str; str++)
    free(*str);

  free(strv);
  return NULL;
}

/* -------------------------------------------------------------------------- */
derive_t strv_length(char *const *strv) {
  derive_t n = 0;

  if (!strv)
    return 0;

  for (; *strv; strv++)
    n++;

  return n;
}

/* -------------------------------------------------------------------------- */
static int sdbus_acquire(sd_bus **bus, sdbus_bind_t type) {
  int r;

  switch (type) {
  case TARGET_LOCAL:
    r = sd_bus_default(bus);
    break;
  case TARGET_LOCAL_USER:
    r = sd_bus_default_user(bus);
    break;
  case TARGET_LOCAL_SYSTEM:
    r = sd_bus_default_system(bus);
    break;
  }
  if (r < 0) {
    ERROR(LOG_KEY "failed to connect bus: %d", r);
    return r;
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int sdbus_close(sd_bus **bus) {
  if (*bus != NULL) {
    sd_bus_close(*bus);
    sd_bus_unref(*bus);
    *bus = NULL;
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static char **sdbus_names(sd_bus *bus, bool activatable) {
  int r;
  char **result = NULL;
  if (activatable)
    r = sd_bus_list_names(bus, NULL, &result);
  else
    r = sd_bus_list_names(bus, &result, NULL);

  switch (r) {
  case 0:
    break;
  case -EINVAL:
    ERROR(LOG_KEY_NAMES "bus or both acquired and activatable were NULL.");
    break;
  case -ENOPKG:
    ERROR(LOG_KEY_NAMES "The bus cannot be resolved.");
    break;
  case -ECHILD:
    ERROR(LOG_KEY_NAMES "The bus was created in a different process.");
    break;
  case -ENOMEM:
    ERROR(LOG_KEY_NAMES "Memory allocation failed.");
    break;
  case -ENOTCONN:
    ERROR(LOG_KEY_NAMES "The bus is not connected.");
    break;
  default:
    ERROR(LOG_KEY_NAMES "failed list names: %d", r);
    break;
  }
  if (r < 0)
    return NULL;

  return result;
}

/* -------------------------------------------------------------------------- */
static int sdbus_count_active(sd_bus *bus, derive_t *unique,
                              derive_t *acquired) {
  char **names = sdbus_names(bus, false);
  char **strv;

  if (names == NULL)
    return -1;

  *unique = 0;
  *acquired = 0;

  for (strv = names; *strv; strv++) {
    if ((*strv)[0] == ':') {
      ++*unique;
    } else {
      ++*acquired;
    }
  }

  strv_free(names);

  return 0;
}

/* -------------------------------------------------------------------------- */
static int sdbus_count_activatable(sd_bus *bus, derive_t *activatable) {
  char **names = sdbus_names(bus, true);
  if (names == NULL)
    return -1;

  *activatable = strv_length(names);
  strv_free(names);

  return 0;
}

/* ************************************************************************** */
/* collection service functions */
/* ************************************************************************** */

static void sdbus_submit(const char *instance, derive_t unique,
                         derive_t acquired, derive_t activatable) {
  DEBUG("%s: unique=%lu, acquired=%lu, activatable=%lu),", instance, unique,
        acquired, activatable);

  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.absolute = unique},
      {.absolute = acquired},
      {.absolute = activatable},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, "sdbus", sizeof(vl.plugin));
  sstrncpy(vl.type, "sdbus", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* -------------------------------------------------------------------------- */
static int sdbus_read(void) {
  derive_t unique;
  derive_t acquried;
  derive_t activatable;

  if (sdbus_count_active(bus_user, &unique, &acquried))
    return -1;
  if (sdbus_count_activatable(bus_user, &activatable))
    return -1;
  sdbus_submit("user", unique, acquried, activatable);

  if (sdbus_count_active(bus_system, &unique, &acquried))
    return -1;
  if (sdbus_count_activatable(bus_system, &activatable))
    return -1;
  sdbus_submit("system", unique, acquried, activatable);

  return 0;
}

/* ************************************************************************** */
/* configuration */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
static int sdbus_config(oconfig_item_t *ci) {
  INFO(LOG_KEY "configuration"); 
  return 0; 
}

/* -------------------------------------------------------------------------- */
static int sdbus_init(void) {
  if (sdbus_acquire(&bus_user, TARGET_LOCAL_USER) != 0) {
    return -1;
  }
  if (sdbus_acquire(&bus_system, TARGET_LOCAL_SYSTEM) != 0) {
    return -1;
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int sdbus_shutdown(void) {
  if (sdbus_close(&bus_system) != 0) {
    return -1;
  }
  if (sdbus_close(&bus_user) != 0) {
    return -1;
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_complex_config("sdbus", sdbus_config);
  plugin_register_init("sdbus", sdbus_init);
  plugin_register_read("sdbus", sdbus_read);
  plugin_register_shutdown("sdbus", sdbus_shutdown);
}
