/**
 * SPDX-FileCopyrightText: 2020 Daimler AG
 **/

#include "collectd.h"

#include "filter_chain.h"
#include "utils/common/common.h"
#include "utils_cache.h"

/* ************************************************************************** */
/* types */
/* ************************************************************************** */

struct mlv_match_s;
typedef struct mlv_match_s mlv_match_t;
struct mlv_match_s {
  gauge_t diff_abs;
  gauge_t diff_rel;
};

/* ************************************************************************** */
/* constants */
/* ************************************************************************** */

#define LOG_KEY "match last value: "

#define SATISFY_ALL 0
#define SATISFY_ANY 1

/* ************************************************************************** */
/* plugin functions */
/* ************************************************************************** */

static int mlv_create(const oconfig_item_t *ci, void **user_data) {
  mlv_match_t *m;
  int status;

  m = calloc(1, sizeof(*m));
  if (m == NULL) {
    ERROR("mlv_create: calloc failed.");
    return -ENOMEM;
  }

  m->diff_abs = NAN;
  m->diff_rel = NAN;

  status = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("DiffAbs", child->key) == 0)
      status = cf_util_get_double(child, &m->diff_abs);
    else if (strcasecmp("DiffRel", child->key) == 0)
      status = cf_util_get_double(child, &m->diff_rel);
    else {
      ERROR(LOG_KEY "The `%s' configuration option is not "
                    "understood and will be ignored.",
            child->key);
      status = 0;
    }

    if (status != 0)
      break;
  }

  if (status != 0) {
    free(m);
    return status;
  }

  if (isnan(m->diff_abs) && isnan(m->diff_rel))
    m->diff_rel = 0.0;

  *user_data = m;
  return 0;
}

/* -------------------------------------------------------------------------- */
static int mlv_destroy(void **user_data) {
  if (user_data != NULL) {
    sfree(*user_data);
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int mlv_match(const data_set_t __attribute__((unused)) * ds,
                     const value_list_t *vl,
                     notification_meta_t __attribute__((unused)) * *meta,
                     void **user_data) {
  mlv_match_t *m;
  gauge_t values_history[2 * 10];
  gauge_t vnew, vold;
  int status;

  if ((user_data == NULL) || (*user_data == NULL))
    return -1;

  status = uc_get_history(ds, vl, values_history, 2, ds->ds_num);
  if (status != 0) {
    WARNING(LOG_KEY "no history available (%d)", status);
    return FC_MATCH_NO_MATCH;
  }

  m = *user_data;
  for (int dco = 0; dco < ds->ds_num; ++dco) {
    vnew = values_history[dco];
    if (isnan(vnew)) {
      DEBUG(LOG_KEY "new value is NAN");
      return FC_MATCH_NO_MATCH;
    }

    vold = values_history[dco + ds->ds_num];
    if (isnan(vold)) {
      DEBUG(LOG_KEY "old value is NAN");
      return FC_MATCH_NO_MATCH;
    }

    gauge_t diff = fabs(vnew - vold);
    if (!isnan(m->diff_abs) && (diff > m->diff_abs)) {
      return FC_MATCH_NO_MATCH;
    }
    if (!isnan(m->diff_rel) && ((diff / fabs(vold)) > m->diff_rel)) {
      return FC_MATCH_NO_MATCH;
    }
  }

  // m = *user_data;

  return FC_MATCH_MATCHES;
}

/* ************************************************************************** */
/* configuration */
/* ************************************************************************** */

void module_register(void) {
  match_proc_t mproc = {0};

  mproc.create = mlv_create;
  mproc.destroy = mlv_destroy;
  mproc.match = mlv_match;
  fc_register_match("last_value", mproc);
}
