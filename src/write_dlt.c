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
#define WL_CONTEXT_MAX 100
#define WL_FORMAT_GRAPHITE 1
#define WL_FORMAT_JSON 2

/* ************************************************************************** */
/* constants */
/* ************************************************************************** */

static int wdlt_format = WL_FORMAT_GRAPHITE;
static char wdlt_appid[] = "CLTD";
static const char* wdlt_name = "write_dlt plugin";

/* ************************************************************************** */
/* dlt context management */
/* ************************************************************************** */


typedef struct wdlt_context_info_s {
  DltContext context;
  struct wdlt_context_info_s* _next;
} wdlt_context_info_t;

static DltContext* jsonContext;
static DltContext* graphiteContext;
static DltContext* jsonContext;

static wdlt_context_info_t* wdlt_contexts;

/* -------------------------------------------------------------------------- */
DltContext* wdlt_context_get(const char* name, const char* description) {
  DltReturnValue dlt_ret;
  wdlt_context_info_t* ci;

  if (name == NULL) {
    name = "NULL";
  }

  /* find existing context */
  for (ci = wdlt_contexts; ci != NULL; ci = ci->_next) {
    if (strncmp(ci->context.contextID, name, 4) == 0) {
      return &ci->context;
    }
  }

  /* create new context */
  ci = calloc(1, sizeof(*ci));
  
  INFO("%s: register DLT context '%s' (%p)", wdlt_name, name, &ci->context);
  dlt_ret = dlt_register_context(&ci->context, name, description);
  if (dlt_ret != DLT_RETURN_OK) {
    ERROR("%s: creating DLT context '%s' failed", wdlt_name, name);
    sfree(ci);
    return NULL;
  }

  if (wdlt_contexts == NULL) {
    wdlt_contexts = ci;
  } else {
    ci->_next = wdlt_contexts;
    wdlt_contexts = ci;
  }

  return &ci->context;
}

/* -------------------------------------------------------------------------- */
void wdlt_context_clear() {
  DltReturnValue dlt_ret;

  while (wdlt_contexts != NULL) {
    wdlt_context_info_t* to_delete = wdlt_contexts;
    wdlt_contexts = to_delete->_next;
    INFO("%s: unregister DLT context '%.4s' (%p)", 
          wdlt_name, to_delete->context.contextID, &to_delete->context);
    dlt_ret = dlt_unregister_context(&to_delete->context);
    if (dlt_ret != DLT_RETURN_OK) {
      ERROR("%s: unregistering DLT context failed", wdlt_name);
    }
    free(to_delete);
  }
}

/* ************************************************************************** */
/* level list */
/* ************************************************************************** */

typedef struct level_entry_s {
#if HAVE_REGEX_H
  regex_t *re;
#endif
  DltLogLevelType dlt_level;
  struct level_entry_s* _next;
} level_entry_t;

static level_entry_t* level_list_begin = NULL;
static level_entry_t* level_list_end = NULL;

/* -------------------------------------------------------------------------- */
static void wdlt_level_list_add(const char* regexp, const char* level) {
    int status;

    level_entry_t* level_entry = malloc(sizeof(level_entry_t));
    if (level_entry == NULL) {
      ERROR("%s: level_list_add: malloc failed.", wdlt_name);
      return ;
    }

#if HAVE_REGEX_H
  if (regexp != NULL) {
    level_entry->re = calloc(1, sizeof(*level_entry->re));
    if (level_entry->re == NULL) {
      ERROR("%s: level_list_add: calloc failed.", wdlt_name);
      sfree(level_entry);
      return;
    }

    status = regcomp(level_entry->re, regexp, REG_EXTENDED | REG_NOSUB);
    if (status != 0) {
      DEBUG("%s: compiling the regular expression \"%s\" failed.",
            wdlt_name, regexp);
      regfree(level_entry->re);
      sfree(level_entry);
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
    sfree(level_entry);
    return;
  }
#endif

    level_entry->dlt_level = DLT_LOG_INFO;
    if (level != NULL) {
      if(strcasecmp(level, "DEFAULT") == 0) {
        level_entry->dlt_level = DLT_LOG_DEFAULT;
      } else if(strcasecmp(level, "OFF") == 0) {
        level_entry->dlt_level = DLT_LOG_OFF;
      } else if(strcasecmp(level, "FATAL") == 0) {
        level_entry->dlt_level = DLT_LOG_FATAL;
      } else if(strcasecmp(level, "ERROR") == 0) {
        level_entry->dlt_level = DLT_LOG_ERROR;
      } else if(strcasecmp(level, "WARN") == 0) {
        level_entry->dlt_level = DLT_LOG_WARN;
      } else if(strcasecmp(level, "INFO") == 0) {
        level_entry->dlt_level = DLT_LOG_INFO;
      } else if(strcasecmp(level, "DEBUG") == 0) {
        level_entry->dlt_level = DLT_LOG_DEBUG;
      } else if(strcasecmp(level, "VERBOSE") == 0) {
        level_entry->dlt_level = DLT_LOG_VERBOSE;
      }
    }
      
    DEBUG("%s: add DLT level match '%s' --> %s (%d)", 
          wdlt_name, regexp, level, level_entry->dlt_level);
    if (level_list_begin == NULL) {
      level_list_begin = level_entry;
      level_list_end = level_entry;
    } else {
      level_list_end->_next = level_entry;      
      level_list_end = level_entry;      
    }

}

/* -------------------------------------------------------------------------- */
static void wdlt_level_list_clear() {
  DEBUG("%s: level_list_clear: begin", wdlt_name);
  while (level_list_begin != NULL) {
    level_entry_t* level_entry_to_delete = level_list_begin;
    level_list_begin = level_entry_to_delete->_next;
    regfree(level_entry_to_delete->re);
    free(level_entry_to_delete);
  }
  level_list_end = NULL;
}

/* -------------------------------------------------------------------------- */
static DltLogLevelType wdlt_level_list_get(const char* message) {
#if HAVE_REGEX_H
  level_entry_t* me;
  for (me = level_list_begin; me != NULL; me = me->_next) {
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


/* ************************************************************************** */
/* context list */
/* ************************************************************************** */

typedef struct context_entry_s {
#if HAVE_REGEX_H
  regex_t *re;
#endif
  DltContext* dlt_context;
  struct context_entry_s* _next;
} context_entry_t;

static context_entry_t* context_list_begin = NULL;
static context_entry_t* context_list_end = NULL;

/* -------------------------------------------------------------------------- */
static void wdlt_context_list_add(const char* regexp, const char* context) {
    int status;

    context_entry_t* context_entry = calloc(1, sizeof(context_entry_t));
    if (context_entry == NULL) {
      ERROR("%s: context_list_add: malloc failed.", wdlt_name);
      return ;
    }

#if HAVE_REGEX_H
  if (regexp != NULL) {
    context_entry->re = calloc(1, sizeof(*context_entry->re));
    if (context_entry->re == NULL) {
      ERROR("%s: context_list_add: calloc failed.", wdlt_name);
      sfree(context_entry);
      return;
    }

    status = regcomp(context_entry->re, regexp, REG_EXTENDED | REG_NOSUB);
    if (status != 0) {
      DEBUG("%s: compiling the regular expression \"%s\" failed.",
            wdlt_name, regexp);
      regfree(context_entry->re);
      sfree(context_entry);
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
    sfree(context_entry);
    return;
  }
#endif

    context_entry->dlt_context = wdlt_context_get(context, "dynamic");
    DEBUG("%s: add DLT context match '%s' --> %s", 
          wdlt_name, regexp, context);
      
    if (context_list_begin == NULL) {
      context_list_begin = context_entry;
      context_list_end = context_entry;
    } else {
      context_list_end->_next = context_entry;      
      context_list_end = context_entry;      
    }

}

/* -------------------------------------------------------------------------- */
static void wdlt_context_list_clear() {
  DEBUG("%s: context_list_clear: begin", wdlt_name);
  while (context_list_begin != NULL) {
    context_entry_t* context_entry_to_delete = context_list_begin;
    context_list_begin = context_entry_to_delete->_next;
    regfree(context_entry_to_delete->re);
    free(context_entry_to_delete);
  }
  context_list_end = NULL;
}

/* -------------------------------------------------------------------------- */
static DltContext* wdlt_context_list_get(const char* message, DltContext* def) {
#if HAVE_REGEX_H
  context_entry_t* me;
  for (me = context_list_begin; me != NULL; me = me->_next) {
    if (me->re == NULL) {
      continue;
    }
    if (regexec(me->re, message, 0, NULL, 0) == 0) {
      return me->dlt_context;
    }
  }
#endif
  return def;
}


/* ************************************************************************** */
/* output functions */
/* ************************************************************************** */

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

  DltLogLevelType dlt_level = wdlt_level_list_get(buffer);
  DltContext* dlt_context = wdlt_context_list_get(buffer, graphiteContext);
  if (dlt_context != NULL) {
    DLT_LOG(*dlt_context, dlt_level, DLT_STRING(buffer));
  }
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
  format_json_value_list(buffer, &bfill, &bfree, ds, vl, 0);
  format_json_finalize(buffer, &bfill, &bfree);

  DltLogLevelType dlt_level = wdlt_level_list_get(buffer);
  DltContext* dlt_context = wdlt_context_list_get(buffer, jsonContext);
  if (dlt_context != NULL) {
    DLT_LOG(*dlt_context, dlt_level, DLT_STRING(buffer));
  }

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


/* ************************************************************************** */
/* configuration */
/* ************************************************************************** */

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
        ERROR("%s: `'MatchLevel' needs exactly two string arguments (got %i).",
              wdlt_name, child->values_num);
        continue;
      }
      wdlt_level_list_add(child->values[0].value.string, 
                          child->values[1].value.string);
    } else if (strcasecmp("MatchContext", child->key) == 0) {
      if ((child->values_num != 2) ||
          (OCONFIG_TYPE_STRING != child->values[0].type) ||
          (OCONFIG_TYPE_STRING != child->values[1].type)) {
        ERROR("%s: `'MatchContext' needs exactly two string arguments (got %i).",
              wdlt_name, child->values_num);
        continue;
      }
      wdlt_context_list_add(child->values[0].value.string, 
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

  jsonContext = wdlt_context_get("JSON", "use json format");
  graphiteContext = wdlt_context_get("GRPH", "use graphite format");

  return 0;
}


/* -------------------------------------------------------------------------- */
static int wdlt_shutdown()
{
  wdlt_level_list_clear();
  wdlt_context_list_clear();
  wdlt_context_clear();

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
