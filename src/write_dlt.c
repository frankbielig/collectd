/**
 * Copyright has to be defined
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include "utils/format_graphite/format_graphite.h"
#include "utils/format_json/format_json.h"

#include "dlt/dlt.h"

#define WL_BUF_SIZE 16384

#define WL_FORMAT_GRAPHITE 1
#define WL_FORMAT_JSON 2


DLT_DECLARE_CONTEXT(jsonContext);
DLT_DECLARE_CONTEXT(graphiteContext);
    
/* Plugin:WriteLog has to also operate without a config, so use a global. */
int wdlt_format = WL_FORMAT_GRAPHITE;
char wdlt_appid[] = "CLTD";
const char* wdlt_name = "write_dlt";

/* -------------------------------------------------------------------------- */
static int wdlt_write_graphite(const data_set_t *ds, const value_list_t *vl) {
  char buffer[WL_BUF_SIZE] = {0};
  int status;

  if (0 != strcmp(ds->type, vl->type)) {
    ERROR("%s: DS type does not match value list type", wdlt_name);
    return -1;
  }

  status = format_graphite(buffer, sizeof(buffer), ds, vl, NULL, NULL, '_', 
                          GRAPHITE_USE_TAGS | GRAPHITE_ALWAYS_APPEND_DS);
  if (status != 0) /* error message has been printed already. */
    return status;

  DLT_LOG(graphiteContext, DLT_LOG_INFO, DLT_STRING(buffer));

  return 0;
} /* int wdlt_write_graphite */


/* -------------------------------------------------------------------------- */
static int wdlt_write_json(const data_set_t *ds, const value_list_t *vl) {
  char buffer[WL_BUF_SIZE] = {0};
  size_t bfree = sizeof(buffer);
  size_t bfill = 0;

  if (0 != strcmp(ds->type, vl->type)) {
    ERROR("%s: DS type does not match value list type", wdlt_name);
    return -1;
  }

  format_json_initialize(buffer, &bfill, &bfree);
  format_json_value_list(buffer, &bfill, &bfree, ds, vl,
                         /* store rates = */ 0);
  format_json_finalize(buffer, &bfill, &bfree);

  DLT_LOG(jsonContext, DLT_LOG_INFO, DLT_STRING(buffer));

  return 0;
} /* int wdlt_write_json */


/* -------------------------------------------------------------------------- */
static int wdlt_write(const data_set_t *ds, const value_list_t *vl,
                    __attribute__((unused)) user_data_t *user_data) {
  int status = 0;

  if (wdlt_format == WL_FORMAT_GRAPHITE) {
    status = wdlt_write_graphite(ds, vl);
  } else if (wdlt_format == WL_FORMAT_JSON) {
    status = wdlt_write_json(ds, vl);
  }

  return status;
}


/* -------------------------------------------------------------------------- */
static int wg_config_dlt(oconfig_item_t *ci)  /* {{{ */
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("AppID", child->key) == 0) {
      bzero(wdlt_appid, 4);
      cf_util_get_string_buffer(child, wdlt_appid, sizeof(wdlt_appid));
    } else {
      ERROR("%s: Invalid configuration option in <DLT>: `%s'.",
            wdlt_name, child->key);
      return -EINVAL;
    }
  }
  return 0;
} /* }}} int wdlt_config_dlt */  


/* -------------------------------------------------------------------------- */
static int wdlt_config(oconfig_item_t *ci) /* {{{ */
{
  bool format_seen = false;
  
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("DLT", child->key) == 0) {
      if (wg_config_dlt(child) == 0)
        continue;
      else
        /* error message written by child function */
        return -EINVAL;
    }

    else if (strcasecmp("Format", child->key) == 0) {
      char str[16];

      if (cf_util_get_string_buffer(child, str, sizeof(str)) != 0)
        continue;

      if (format_seen) {
        WARNING("%s: Redefining option `%s'.", wdlt_name, child->key);
      }
      format_seen = true;

      if (strcasecmp("Graphite", str) == 0)
        wdlt_format = WL_FORMAT_GRAPHITE;
      else if (strcasecmp("JSON", str) == 0)
        wdlt_format = WL_FORMAT_JSON;
      else {
        ERROR("%s: Unknown format `%s' for option `%s'.", str,
              wdlt_name, child->key);
        return -EINVAL;
      }
    } else {
      ERROR("%s: Invalid configuration option: `%s'.",
            wdlt_name, child->key);
      return -EINVAL;
    }
  }

  return 0;
} /* }}} int wdlt_config */


/* -------------------------------------------------------------------------- */
static int wdlt_init() /* {{{ */
{
  INFO("write_dlt: register app with '%s'.", wdlt_appid);
  DLT_REGISTER_APP(wdlt_appid, "Diagnostic Log and Trace");

  DLT_REGISTER_CONTEXT(jsonContext, "JSON", "use json format");
  DLT_REGISTER_CONTEXT(graphiteContext, "GRPH", "use graphite format");

  return 0;
} /* }}} int wdlt_init */


/* -------------------------------------------------------------------------- */
static int wdlt_shutdown() /* {{{ */
{
  DLT_UNREGISTER_CONTEXT(jsonContext);
  DLT_UNREGISTER_CONTEXT(graphiteContext);

  INFO("write_dlt: unregister app with '%s'.", wdlt_appid);
  DLT_UNREGISTER_APP();
  return 0;
} /* }}} int wdlt_shutdown */


/* -------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_complex_config("write_dlt", wdlt_config);
  /* If config is supplied, the global wdlt_format will be set. */
  plugin_register_write("write_dlt", wdlt_write, NULL);

  plugin_register_init ("my_plugin", wdlt_init);
  plugin_register_shutdown ("my_plugin", wdlt_shutdown);
}
