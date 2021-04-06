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

/* ------------------------------------------------------------------------- */
typedef struct sdbus_latency_s {

  gauge_t value;
  latency_counter_t *history;

} sdbus_latency_t;

/* ------------------------------------------------------------------------- */
typedef struct sdbus_metric_s {

  sdbus_latency_t user_local_latency;
  sdbus_latency_t user_peer_latency;
  sdbus_latency_t system_local_latency;
  sdbus_latency_t system_peer_latency;

} sdbus_metric_t;

/* ------------------------------------------------------------------------- */
typedef struct server_info_s {
  sd_bus **bus;
  sdbus_bind_t bus_type;
  pthread_t thread;
  bool running;
  bool shutdown;
} server_info_t;

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

#define PEER_SERVICE "org.freedesktop.DBus"
#define PEER_OBJECT "/org/freedesktop/DBus"
#define PEER_INTERFACE "org.freedesktop.DBus.Peer"
#define PEER_METHOD_PING "Ping"

#define PLUGIN_KEY "sdbus"

/* ************************************************************************* */
/* global variables */
/* ************************************************************************* */

static sd_bus *bus_user = NULL;
static sd_bus *bus_system = NULL;

static server_info_t user_server = {.bus = &bus_user,
                                    .bus_type = TARGET_LOCAL_USER,
                                    .running = false,
                                    .shutdown = false};
static server_info_t system_server = {.bus = &bus_system,
                                      .bus_type = TARGET_LOCAL_SYSTEM,
                                      .running = false,
                                      .shutdown = false};

static sdbus_metric_t *sdbus_metric = 0;

/* ************************************************************************* */
/* helper functions */
/* ************************************************************************* */

static sdbus_metric_t *sdbus_metric_create() {
  sdbus_metric_t *metric = calloc(1, sizeof(*metric));
  if (metric == NULL) {
    ERROR(LOG_KEY "calloc of mectric failed.");
    return NULL;
  }
  metric->user_local_latency.history = latency_counter_create();
  metric->user_peer_latency.history = latency_counter_create();

  return metric;
}

/* ------------------------------------------------------------------------- */
static void sdbus_metric_destroy(sdbus_metric_t **metric) {
  if (metric == NULL)
    return;
  if (*metric == NULL)
    return;
  latency_counter_destroy((*metric)->user_local_latency.history);
  latency_counter_destroy((*metric)->user_peer_latency.history);

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
static const char *sdbus_error_message(const sd_bus_error *e, int error) {

  if (e) {
    if (sd_bus_error_has_name(e, SD_BUS_ERROR_ACCESS_DENIED))
      return "Access denied";

    if (e->message)
      return e->message;
  }

  return strerror(abs(error));
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
  server_info_t *info = (server_info_t *)args;
  int r;

  if (sdbus_acquire(&bus, info->bus_type) != 0) {
    WARNING(LOG_KEY_SERVER "#%d could not connect to system bus",
            info->bus_type);
    pthread_exit(NULL);
  }

  r = sd_bus_add_object_vtable(bus, &slot, SERVER_OBJECT, SERVER_INTERFACE,
                               server_vtable, NULL);
  if (r < 0) {
    WARNING(LOG_KEY_SERVER "#%d failed to add object: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }

  r = sd_bus_request_name(bus, SERVER_SERVICE, 0);
  if (r < 0) {
    WARNING(LOG_KEY_SERVER "#%d failed to acquire service name: %s",
            info->bus_type, strerror(-r));
    goto finish;
  }

  info->running = true;
  while (!info->shutdown) {
    DEBUG(LOG_KEY_SERVER "#%d process", info->bus_type);
    r = sd_bus_process(bus, NULL);
    if (r < 0) {
      WARNING(LOG_KEY_SERVER "#%d failed to process bus: %s", info->bus_type,
              strerror(-r));
      goto finish;
    }
    if (r > 0)
      continue;

    DEBUG(LOG_KEY_SERVER "#%d wait", info->bus_type);
    r = sd_bus_wait(bus, (uint64_t)-1);
    if (r < 0) {
      WARNING(LOG_KEY_SERVER "#%d failed to wait on bus: %s", info->bus_type,
              strerror(-r));
      goto finish;
    }
  }
finish:
  WARNING(LOG_KEY_SERVER "#%d finished", info->bus_type);
  sd_bus_slot_unref(slot);
  sd_bus_unref(bus);
  return NULL;
}

/* ------------------------------------------------------------------------- */
static void server_start(server_info_t *info) {

  if (*info->bus && !info->running) {
    DEBUG(LOG_KEY_SERVER "#%d create thread", info->bus_type);
    int status;
    status = pthread_create(&info->thread, NULL, server_main, info);
    if (status != 0) {
      ERROR(LOG_KEY_SERVER "#%d could not start thread", info->bus_type);
      return;
    }
    DEBUG(LOG_KEY_SERVER "#%d running", info->bus_type);
  }
}

/* ------------------------------------------------------------------------- */
static void server_shutdown(server_info_t *info) {

  if (info->running) {
    DEBUG(LOG_KEY_SERVER "#%d start shutdown sequence", info->bus_type);
    info->shutdown = true;
    pthread_kill(info->thread, SIGTERM);
    pthread_join(info->thread, NULL);
    DEBUG(LOG_KEY_SERVER "#%d shutdown completed", info->bus_type);
  }
  info->running = false;
}

/* ************************************************************************* */
/* collection service functions */
/* ************************************************************************* */

static void sdbus_submit_count(const char *instance, derive_t unique,
                               derive_t acquired, derive_t activatable) {
  DEBUG(LOG_KEY "%s bus - unique=%lu, acquired=%lu, activatable=%lu),",
        instance, unique, acquired, activatable);

  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.gauge = unique},
      {.gauge = acquired},
      {.gauge = activatable},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, PLUGIN_KEY, sizeof(vl.plugin));
  sstrncpy(vl.type, "sdbus_count", sizeof(vl.type));
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

  sstrncpy(vl.plugin, PLUGIN_KEY, sizeof(vl.plugin));
  sstrncpy(vl.type, "sdbus_latency", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* ------------------------------------------------------------------------- */
static cdtime_t sdbus_call(sd_bus *bus, const char *service, const char *object,
                           const char *interface, const char *method) {

  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *m = NULL;
  cdtime_t latency = ~0;
  int r;

  if (bus == NULL) {
    ERROR(LOG_KEY "call of 'busctl call %s %s %s %s' failed with invalid bus",
          service, object, interface, method);
    return 0;
  }


  cdtime_t start = cdtime();
  r = sd_bus_call_method(bus, service, object, interface, method, &error, &m,
                         "");
  if (r >= 0) {
    latency = cdtime() - start;
  } else {
    ERROR(LOG_KEY "call of 'busctl call %s %s %s %s' failed with %d (%s)",
          service, object, interface, method, r,
          sdbus_error_message(&error, r));
  }

  sd_bus_error_free(&error);
  sd_bus_message_unref(m);

  return latency;
}

/* ------------------------------------------------------------------------- */
static void sdbus_latency(server_info_t *server_info, const char *service,
                          const char *object, const char *interface,
                          const char *method, const char *key,
                          sdbus_latency_t *metric) {
  if (!server_info->running)
    return;

  if (server_info->bus == NULL)
    return;

  cdtime_t latency =
      sdbus_call(*server_info->bus, service, object, interface, method);
  if (latency == ~0) {
    return;
  }

  metric->value = CDTIME_T_TO_US(latency);
  latency_counter_add(metric->history, latency);
  DEBUG(LOG_KEY "%s latency %.0fms", key, metric->value);

  sdbus_latency_submit(key, metric->value, metric->history);
}

/* ------------------------------------------------------------------------- */
static int sdbus_read(void) {

  sdbus_count();
  sdbus_latency(&user_server, SERVER_SERVICE, SERVER_OBJECT, SERVER_INTERFACE,
                SERVER_METHOD_PING, "user-local",
                &sdbus_metric->user_local_latency);
  sdbus_latency(&user_server, PEER_SERVICE, PEER_OBJECT, PEER_INTERFACE,
                PEER_METHOD_PING, "user-peer",
                &sdbus_metric->user_peer_latency);
  sdbus_latency(&system_server, SERVER_SERVICE, SERVER_OBJECT, SERVER_INTERFACE,
                SERVER_METHOD_PING, "system-local",
                &sdbus_metric->system_local_latency);
  sdbus_latency(&system_server, PEER_SERVICE, PEER_OBJECT, PEER_INTERFACE,
                PEER_METHOD_PING, "system-peer",
                &sdbus_metric->system_peer_latency);

  return 0;
}

/* ************************************************************************* */
/* configuration */
/* ************************************************************************* */

/* ------------------------------------------------------------------------- */
static int sdbus_config(oconfig_item_t *ci) {
  INFO(LOG_KEY "configuration");
  return 0;
}

/* ------------------------------------------------------------------------- */
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

  server_start(&user_server);
  server_start(&system_server);

  return 0;
}

/* ------------------------------------------------------------------------- */
static int sdbus_shutdown(void) {
  if (bus_system != NULL && sdbus_close(&bus_system) != 0) {
    return -1;
  }
  if (bus_user != NULL && sdbus_close(&bus_user) != 0) {
    return -1;
  }

  server_shutdown(&system_server);
  server_shutdown(&user_server);

  sdbus_metric_destroy(&sdbus_metric);

  return 0;
}

/* ------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_complex_config(PLUGIN_KEY, sdbus_config);
  plugin_register_init(PLUGIN_KEY, sdbus_init);
  plugin_register_read(PLUGIN_KEY, sdbus_read);
  plugin_register_shutdown(PLUGIN_KEY, sdbus_shutdown);
}
