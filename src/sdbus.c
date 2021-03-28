/**
 * SPDX-FileCopyrightText: 2020 Daimler AG
 **/

#include "plugin.h"
#include "utils/common/common.h"
#include "utils_time.h"
#include "collectd.h"

#include "daemon/utils_time.h"
#include "systemd/sd-bus.h"
#include "utils/latency/latency.h"

/* ************************************************************************* */
/* types */
/* ************************************************************************* */

typedef enum {
  TARGET_LOCAL = 0,
  TARGET_LOCAL_USER = 1,
  TARGET_LOCAL_SYSTEM = 2
} sdbus_bind_t;

typedef struct sdbus_metric_s {

  latency_counter_t *ping_latency_hist;
  gauge_t ping_latency_value;

} sdbus_metric_t;

/* ************************************************************************* */
/* constants */
/* ************************************************************************* */

#define LOG_KEY "sdbus: "
#define LOG_KEY_NAMES LOG_KEY "sdbus_names - "
#define LOG_KEY_SERVER LOG_KEY "server - "

#define SERVER_SERVICE "org.collectd.SDBus"
#define SERVER_OBJECT "/org/collectd/SDBus"
#define SERVER_INTERFACE "org.collectd.SDBus"
#define SERVER_METHOD_PING "Ping"

/* ************************************************************************* */
/* global variables */
/* ************************************************************************* */

static sd_bus *bus_user = NULL;
static sd_bus *bus_system = NULL;

static pthread_t server_thread;
static bool server_running = false;
static bool server_shutdown = false;

static sdbus_metric_t *sdbus_metric = 0;

/* ************************************************************************* */
/* helper functions */
/* ************************************************************************* */

/* ------------------------------------------------------------------------- */
static sdbus_metric_t *sdbus_metric_create() {
  sdbus_metric_t *metric = calloc(1, sizeof(*metric));
  if (metric == NULL) {
    ERROR(LOG_KEY "calloc of mectric failed.");
    return NULL;
  }
  metric->ping_latency_hist = latency_counter_create();

  return metric;
}

/* ------------------------------------------------------------------------- */
static void sdbus_metric_destroy(sdbus_metric_t **metric) {
  if (metric == NULL)
    return;
  if (*metric == NULL)
    return;
  latency_counter_destroy((*metric)->ping_latency_hist);

  sfree(*metric);
}

/* ------------------------------------------------------------------------- */
static char **strv_free(char **strv) {
  char **str;

  if (!strv)
    return NULL;

  for (str = strv; *str; str++)
    free(*str);

  free(strv);
  return NULL;
}

/* ------------------------------------------------------------------------- */
static derive_t strv_length(char *const *strv) {
  derive_t n = 0;

  if (!strv)
    return 0;

  for (; *strv; strv++)
    n++;

  return n;
}

/* ------------------------------------------------------------------------- */
static int sdbus_acquire(sd_bus **bus, sdbus_bind_t type) {
  int r = -1;

  *bus = NULL;
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
  default:
    ERROR(LOG_KEY "invalid bus type %d", type);
    return -1;
  }
  if (r < 0) {
    ERROR(LOG_KEY "failed to connect bus %d with %d", type, r);
    return r;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
static int sdbus_close(sd_bus **bus) {
  if (*bus != NULL) {
    sd_bus_close(*bus);
    sd_bus_unref(*bus);
    *bus = NULL;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
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

/* ------------------------------------------------------------------------- */
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

/* ------------------------------------------------------------------------- */
static int sdbus_count_activatable(sd_bus *bus, derive_t *activatable) {
  char **names = sdbus_names(bus, true);
  if (names == NULL)
    return -1;

  *activatable = strv_length(names);
  strv_free(names);

  return 0;
}

/* ************************************************************************* */
/* sdbus server */
/* ************************************************************************* */

static int server_method_ping(sd_bus_message *m, void *userdata,
                              sd_bus_error *ret_error) {

  INFO(LOG_KEY_SERVER "ping");
  return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable server_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD(SERVER_METHOD_PING, "", "", server_method_ping,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

/* ------------------------------------------------------------------------- */
static void *server_main(void *args) {
  sd_bus_slot *slot = NULL;
  sd_bus *bus = NULL;
  int r;

  if (sdbus_acquire(&bus, TARGET_LOCAL_USER) != 0) {
    WARNING(LOG_KEY_SERVER "could not connect to system bus");
    pthread_exit(NULL);
  }

  r = sd_bus_add_object_vtable(bus, &slot, SERVER_OBJECT, SERVER_INTERFACE,
                               server_vtable, NULL);
  if (r < 0) {
    WARNING(LOG_KEY_SERVER "failed to add object: %s", strerror(-r));
    goto finish;
  }

  r = sd_bus_request_name(bus, SERVER_SERVICE, 0);
  if (r < 0) {
    WARNING(LOG_KEY_SERVER "failed to acquire service name: %s", strerror(-r));
    goto finish;
  }

  while (!server_shutdown) {
    DEBUG(LOG_KEY_SERVER "process");
    r = sd_bus_process(bus, NULL);
    if (r < 0) {
      WARNING(LOG_KEY_SERVER "failed to process bus: %s", strerror(-r));
      goto finish;
    }
    if (r > 0)
      continue;

    DEBUG(LOG_KEY_SERVER "wait");
    r = sd_bus_wait(bus, (uint64_t)-1);
    if (r < 0) {
      WARNING(LOG_KEY_SERVER "failed to wait on bus: %s", strerror(-r));
      goto finish;
    }
  }
finish:
  WARNING(LOG_KEY_SERVER "finished");
  sd_bus_slot_unref(slot);
  sd_bus_unref(bus);
  return NULL;
}

/* ************************************************************************* */
/* collection service functions */
/* ************************************************************************* */

static void sdbus_submit_count(const char *instance, derive_t unique,
                               derive_t acquired, derive_t activatable) {
  DEBUG("%s: unique=%lu, acquired=%lu, activatable=%lu),", instance, unique,
        acquired, activatable);

  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.gauge = unique},
      {.gauge = acquired},
      {.gauge = activatable},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, "sdbus", sizeof(vl.plugin));
  sstrncpy(vl.type, "size", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* ------------------------------------------------------------------------- */
static int sdbus_count(void) {
  derive_t unique;
  derive_t acquried;
  derive_t activatable;

  if (bus_user != NULL) {
    if (sdbus_count_active(bus_user, &unique, &acquried))
      return -1;
    if (sdbus_count_activatable(bus_user, &activatable))
      return -1;
    sdbus_submit_count("user", unique, acquried, activatable);
  }

  if (bus_system != NULL) {
    if (sdbus_count_active(bus_system, &unique, &acquried))
      return -1;
    if (sdbus_count_activatable(bus_system, &activatable))
      return -1;
    sdbus_submit_count("system", unique, acquried, activatable);
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
static void sdbus_latency_submit(const char *instance, gauge_t value,
                                 latency_counter_t *hist) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.gauge = value},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, "sdbus", sizeof(vl.plugin));
  sstrncpy(vl.type, "latency", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* -------------------------------------------------------------------------
 */
static void sdbus_benchmark(void) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *m = NULL;
  int r;

  cdtime_t start = cdtime();
  r = sd_bus_call_method(bus_user, SERVER_SERVICE, /* service to contact */
                         SERVER_OBJECT,            /* object path */
                         SERVER_INTERFACE,         /* interface name */
                         SERVER_METHOD_PING,       /* method name */
                         &error, /* object to return error in */
                         &m,     /* return message on success */
                         "");    /* input signature */

  if (r < 0) {
    fprintf(stderr, "Failed to issue method call: %s\n", error.message);
    goto finish;
  }

  cdtime_t ping_latency = cdtime() - start;
  sdbus_metric->ping_latency_value = CDTIME_T_TO_US(ping_latency);
  latency_counter_add(sdbus_metric->ping_latency_hist, ping_latency);
  DEBUG(LOG_KEY "latency %.0fms", sdbus_metric->ping_latency_value);

  sdbus_latency_submit("ping", sdbus_metric->ping_latency_value,
                       sdbus_metric->ping_latency_hist);

finish:
  sd_bus_error_free(&error);
  sd_bus_message_unref(m);
}

/* -------------------------------------------------------------------------
 */
static int sdbus_read(void) {

  sdbus_count();
  sdbus_benchmark();

  return 0;
}

/* *************************************************************************
 */
/* configuration */
/* *************************************************************************
 */

/* -------------------------------------------------------------------------
 */
static int sdbus_config(oconfig_item_t *ci) {
  INFO(LOG_KEY "configuration");
  return 0;
}

/* -------------------------------------------------------------------------
 */
static int sdbus_init(void) {

  sdbus_metric = sdbus_metric_create();
  if (!sdbus_metric)
    return -1;

  if (sdbus_acquire(&bus_user, TARGET_LOCAL_USER) != 0) {
    WARNING(LOG_KEY "could not connect to user bus");
  }
  if (sdbus_acquire(&bus_system, TARGET_LOCAL_SYSTEM) != 0) {
    WARNING(LOG_KEY "could not connect to system bus");
  }

  if (!server_running) {
    DEBUG(LOG_KEY_SERVER "create thread");
    int status;
    status = pthread_create(&server_thread, NULL, server_main, NULL);
    if (status != 0) {
      ERROR(LOG_KEY_SERVER "could not start thread");
      return -1;
    }
    server_running = true;
    DEBUG(LOG_KEY_SERVER "running");
  }

  return 0;
}

/* -------------------------------------------------------------------------
 */
static int sdbus_shutdown(void) {
  if (bus_system != NULL && sdbus_close(&bus_system) != 0) {
    return -1;
  }
  if (bus_user != NULL && sdbus_close(&bus_user) != 0) {
    return -1;
  }

  if (server_running) {
    DEBUG(LOG_KEY_SERVER "start shutdown sequence");
    server_shutdown = true;
    pthread_kill(server_thread, SIGTERM);
    pthread_join(server_thread, NULL);
    DEBUG(LOG_KEY_SERVER "shutdown completed");
  }
  server_running = false;

  sdbus_metric_destroy(&sdbus_metric);

  return 0;
}

/* -------------------------------------------------------------------------
 */
void module_register(void) {
  plugin_register_complex_config("sdbus", sdbus_config);
  plugin_register_init("sdbus", sdbus_init);
  plugin_register_read("sdbus", sdbus_read);
  plugin_register_shutdown("sdbus", sdbus_shutdown);
}
