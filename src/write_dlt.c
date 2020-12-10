/**
 * SPDX-FileCopyrightText: 2020 Daimler AG
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include "utils/format_graphite/format_graphite.h"
#include "utils/format_json/format_json.h"

#include "dlt/dlt.h"

#if HAVE_REGEX_H
#include <regex.h>
#endif

#define WL_BUF_SIZE 16384
#define WL_FORMAT_GRAPHITE 1
#define WL_FORMAT_JSON 2

DLT_DECLARE_CONTEXT(jsonContext);
DLT_DECLARE_CONTEXT(graphiteContext);
    
/* -------------------------------------------------------------------------- */
/* dlt global variables */

int wdlt_format = WL_FORMAT_GRAPHITE;
char wdlt_appid[] = "CLTD";
const char* wdlt_name = "write_dlt plugin";

/* -------------------------------------------------------------------------- */
/* matching list */
typedef struct match_entry_s {
#if HAVE_REGEX_H
  regex_t *re;
#endif
  DltLogLevelType dlt_level;
  struct match_entry_s* _next;
} match_entry_t;

static match_entry_t* match_list_begin = NULL;
static match_entry_t* match_list_end = NULL;

/* -------------------------------------------------------------------------- */
static void wdlt_match_list_add(const char* regexp, const char* level) {
    int status;

    match_entry_t* match_entry = malloc(sizeof(match_entry_t));
    if (match_entry == NULL) {
      ERROR("%s: match_list_add: malloc failed.", wdlt_name);
      return ;
    }

#if HAVE_REGEX_H
  if (regexp != NULL) {
    match_entry->re = calloc(1, sizeof(*match_entry->re));
    if (match_entry->re == NULL) {
      ERROR("%s: match_list_add: calloc failed.", wdlt_name);
      sfree(match_entry);
      return;
    }

    status = regcomp(match_entry->re, regexp, REG_EXTENDED | REG_NOSUB);
    if (status != 0) {
      DEBUG("%s: compiling the regular expression \"%s\" failed.",
            mdlt_name, regexp);
      sfree(match_entry->re);
      sfree(match_entry);
      return;
    }
  }
#else
  if (regexp != NULL) {
    ERROR("%s: ps_list_register: "
          "Regular expression \"%s\" found in config "
          "file, but support for regular expressions "
          "has been disabled at compile time.",
          wdlt_name, regexp);
    sfree(match_entry);
    return;
  }
#endif

    match_entry->dlt_level = DLT_LOG_INFO;
    if (level != NULL) {
      if(strcasecmp(level, "DEFAULT") == 0) {
        match_entry->dlt_level = DLT_LOG_DEFAULT;
      } else if(strcasecmp(level, "OFF") == 0) {
        match_entry->dlt_level = DLT_LOG_OFF;
      } else if(strcasecmp(level, "FATAL") == 0) {
        match_entry->dlt_level = DLT_LOG_FATAL;
      } else if(strcasecmp(level, "ERROR") == 0) {
        match_entry->dlt_level = DLT_LOG_ERROR;
      } else if(strcasecmp(level, "WARN") == 0) {
        match_entry->dlt_level = DLT_LOG_WARN;
      } else if(strcasecmp(level, "INFO") == 0) {
        match_entry->dlt_level = DLT_LOG_INFO;
      } else if(strcasecmp(level, "DEBUG") == 0) {
        match_entry->dlt_level = DLT_LOG_DEBUG;
      } else if(strcasecmp(level, "VERBOSE") == 0) {
        match_entry->dlt_level = DLT_LOG_VERBOSE;
      }
    }
      
    if (match_list_begin == NULL) {
      match_list_begin = match_entry;
      match_list_end = match_entry;
    } else {
      match_list_end->_next = match_entry;      
      match_list_end = match_entry;      
    }

}

/* -------------------------------------------------------------------------- */
static void wdlt_match_list_clear() {
  while (match_list_begin != NULL) {
    match_entry_t* match_entry_to_delete = match_list_begin;
    match_list_begin = match_entry_to_delete->_next;
  }
  match_list_end = NULL;
}

/* -------------------------------------------------------------------------- */
static DltLogLevelType wdlt_match_list_get_dlt_level(const char* message) {
#if HAVE_REGEX_H
  match_entry_t* me;
  for (me = match_list_begin; me != NULL; me = me->_next) {
    if (me->re == NULL) {
      continue;
    }
    if (regexec(me->re, message, 0, NULL, 0) == 0) {
      return me->dlt_level;
    }
  }
#endif
  return DLT_LOG_INFO;
}


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

  DltLogLevelType dlt_level = wdlt_match_list_get_dlt_level(buffer);
  DLT_LOG(graphiteContext, dlt_level, DLT_STRING(buffer));

  return 0;
}


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

  DltLogLevelType dlt_level = wdlt_match_list_get_dlt_level(buffer);
  DLT_LOG(jsonContext, dlt_level, DLT_STRING(buffer));

  return 0;
}


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
static int wg_config_dlt(oconfig_item_t *ci) 
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("AppID", child->key) == 0) {
      bzero(wdlt_appid, 4);
      cf_util_get_string_buffer(child, wdlt_appid, sizeof(wdlt_appid));
    } else if (strcasecmp("MatchLevel", child->key) == 0) {
      if ((child->values_num != 2) ||
          (OCONFIG_TYPE_STRING != child->values[0].type) ||
          (OCONFIG_TYPE_STRING != child->values[1].type)) {
        ERROR("%s: `ProcessMatch' needs exactly two string arguments (got %i).",
              wdlt_name, child->values_num);
        continue;
      }
      wdlt_match_list_add(child->values[0].value.string, 
                          child->values[1].value.string);
    } else {
      ERROR("%s: Invalid configuration option in <DLT>: `%s'.",
            wdlt_name, child->key);
      return -EINVAL;
    }
  }
  return 0;
}


/* -------------------------------------------------------------------------- */
static int wdlt_config(oconfig_item_t *ci)
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
}


/* -------------------------------------------------------------------------- */
static int wdlt_init()
{
  INFO("write_dlt: register app with '%s'.", wdlt_appid);
  DLT_REGISTER_APP(wdlt_appid, "Diagnostic Log and Trace");

  DLT_REGISTER_CONTEXT(jsonContext, "JSON", "use json format");
  DLT_REGISTER_CONTEXT(graphiteContext, "GRPH", "use graphite format");

  return 0;
}


/* -------------------------------------------------------------------------- */
static int wdlt_shutdown()
{
  wdlt_match_list_clear();

  DLT_UNREGISTER_CONTEXT(jsonContext);
  DLT_UNREGISTER_CONTEXT(graphiteContext);

  INFO("write_dlt: unregister app with '%s'.", wdlt_appid);
  DLT_UNREGISTER_APP();
  return 0;
} 


/* -------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_complex_config("write_dlt", wdlt_config);
  plugin_register_write("write_dlt", wdlt_write, NULL);

  plugin_register_init ("my_plugin", wdlt_init);
  plugin_register_shutdown ("my_plugin", wdlt_shutdown);
}
