/**
 * collectd - src/curl.c
 * Copyright (C) 2006-2009  Florian octo Forster
 * Copyright (C) 2009       Aman Gupta
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 *   Aman Gupta <aman at tmm1.net>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/curl_stats/curl_stats.h"
#include "utils/match/match.h"
#include "utils_time.h"

#include <curl/curl.h>

/*
 * Data types
 */
struct web_match_s;
typedef struct web_match_s web_match_t;
struct web_match_s /* {{{ */
{
  char *regex;
  char *exclude_regex;
  int dstype;
  char *metric;
  char *help;
  label_set_t labels;
  metric_t tmpl;
  cu_match_t *match;

  web_match_t *next;
}; /* }}} */

struct web_page_s;
typedef struct web_page_s web_page_t;
struct web_page_s /* {{{ */
{
  char *instance;
  char *metric_prefix;
  label_set_t labels;
  metric_t tmpl;
  char *metric_response_time;
  char *metric_response_code;

  char *url;
  int address_family;
  char *user;
  char *pass;
  char *credentials;
  bool digest;
  bool verify_peer;
  bool verify_host;
  char *cacert;
  struct curl_slist *headers;
  char *post_body;
  bool response_time;
  bool response_code;
  int timeout;
  curl_stats_t *stats;

  CURL *curl;
  char curl_errbuf[CURL_ERROR_SIZE];
  char *buffer;
  size_t buffer_size;
  size_t buffer_fill;

  web_match_t *matches;
}; /* }}} */

/*
 * Private functions
 */
static int cc_read_page(user_data_t *ud);

static size_t cc_curl_callback(void *buf, /* {{{ */
                               size_t size, size_t nmemb, void *user_data) {
  web_page_t *wp;
  size_t len;

  len = size * nmemb;
  if (len == 0)
    return len;

  wp = user_data;
  if (wp == NULL)
    return 0;

  if ((wp->buffer_fill + len) >= wp->buffer_size) {
    char *temp;
    size_t temp_size;

    temp_size = wp->buffer_fill + len + 1;
    temp = realloc(wp->buffer, temp_size);
    if (temp == NULL) {
      ERROR("curl plugin: realloc failed.");
      return 0;
    }
    wp->buffer = temp;
    wp->buffer_size = temp_size;
  }

  memcpy(wp->buffer + wp->buffer_fill, (char *)buf, len);
  wp->buffer_fill += len;
  wp->buffer[wp->buffer_fill] = 0;

  return len;
} /* }}} size_t cc_curl_callback */

static void cc_web_match_free(web_match_t *wm) /* {{{ */
{
  if (wm == NULL)
    return;

  sfree(wm->regex);
  sfree(wm->exclude_regex);
  sfree(wm->metric);
  sfree(wm->help);
  label_set_reset(&wm->labels);
  metric_reset(&wm->tmpl);
  match_destroy(wm->match);
  cc_web_match_free(wm->next);
  sfree(wm);
} /* }}} void cc_web_match_free */

static void cc_web_page_free(void *arg) /* {{{ */
{
  web_page_t *wp = (web_page_t *)arg;
  if (wp == NULL)
    return;

  if (wp->curl != NULL)
    curl_easy_cleanup(wp->curl);
  wp->curl = NULL;

  sfree(wp->metric_prefix);
  label_set_reset(&wp->labels);
  metric_reset(&wp->tmpl);
  sfree(wp->metric_response_time);
  sfree(wp->metric_response_code);
  sfree(wp->url);
  sfree(wp->user);
  sfree(wp->pass);
  sfree(wp->credentials);
  sfree(wp->cacert);
  sfree(wp->post_body);
  curl_slist_free_all(wp->headers);
  curl_stats_destroy(wp->stats);

  sfree(wp->buffer);

  cc_web_match_free(wp->matches);
  sfree(wp);
} /* }}} void cc_web_page_free */

static int cc_config_append_string(const char *name,
                                   struct curl_slist **dest, /* {{{ */
                                   oconfig_item_t *ci) {
  struct curl_slist *temp = NULL;
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("curl plugin: `%s' needs exactly one string argument.", name);
    return -1;
  }

  temp = curl_slist_append(*dest, ci->values[0].value.string);
  if (temp == NULL)
    return -1;

  *dest = temp;

  return 0;
} /* }}} int cc_config_append_string */

static int cc_config_add_match_dstype(int *dstype_ret, /* {{{ */
                                      oconfig_item_t *ci) {
  int dstype;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("curl plugin: `DSType' needs exactly one string argument.");
    return -1;
  }

  if (strncasecmp("Gauge", ci->values[0].value.string, strlen("Gauge")) == 0) {
    dstype = UTILS_MATCH_DS_TYPE_GAUGE;
    if (strcasecmp("GaugeAverage", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_GAUGE_AVERAGE;
    else if (strcasecmp("GaugeMin", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_GAUGE_MIN;
    else if (strcasecmp("GaugeMax", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_GAUGE_MAX;
    else if (strcasecmp("GaugeLast", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_GAUGE_LAST;
    else
      dstype = 0;
  } else if (strncasecmp("Counter", ci->values[0].value.string,
                         strlen("Counter")) == 0) {
    dstype = UTILS_MATCH_DS_TYPE_COUNTER;
    if (strcasecmp("CounterSet", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_COUNTER_SET;
    else if (strcasecmp("CounterAdd", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_COUNTER_ADD;
    else if (strcasecmp("CounterInc", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_COUNTER_INC;
    else
      dstype = 0;
  } else if (strncasecmp("Derive", ci->values[0].value.string,
                         strlen("Derive")) == 0) {
    dstype = UTILS_MATCH_DS_TYPE_DERIVE;
    if (strcasecmp("DeriveSet", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_DERIVE_SET;
    else if (strcasecmp("DeriveAdd", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_DERIVE_ADD;
    else if (strcasecmp("DeriveInc", ci->values[0].value.string) == 0)
      dstype |= UTILS_MATCH_CF_DERIVE_INC;
    else
      dstype = 0;
  }

  else {
    dstype = 0;
  }

  if (dstype == 0) {
    WARNING("curl plugin: `%s' is not a valid argument to `DSType'.",
            ci->values[0].value.string);
    return -1;
  }

  *dstype_ret = dstype;
  return 0;
} /* }}} int cc_config_add_match_dstype */

static int cc_config_add_match(web_page_t *page, /* {{{ */
                               oconfig_item_t *ci) {
  web_match_t *match;
  int status;

  if (ci->values_num != 0) {
    WARNING("curl plugin: Ignoring arguments for the `Match' block.");
  }

  match = calloc(1, sizeof(*match));
  if (match == NULL) {
    ERROR("curl plugin: calloc failed.");
    return -1;
  }
  status = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Regex", child->key) == 0)
      status = cf_util_get_string(child, &match->regex);
    else if (strcasecmp("ExcludeRegex", child->key) == 0)
      status = cf_util_get_string(child, &match->exclude_regex);
    else if (strcasecmp("DSType", child->key) == 0)
      status = cc_config_add_match_dstype(&match->dstype, child);
    else if (strcasecmp("Metric", child->key) == 0)
      status = cf_util_get_string(child, &match->metric);
    else if (strcasecmp("Help", child->key) == 0)
      status = cf_util_get_string(child, &match->help);
    else if (strcasecmp("Label", child->key) == 0)
      status = cf_util_get_label(child, &match->labels);
    else {
      WARNING("curl plugin: Option `%s' not allowed here.", child->key);
      status = -1;
    }

    if (status != 0)
      break;
  } /* for (i = 0; i < ci->children_num; i++) */

  while (status == 0) {
    if (match->regex == NULL) {
      WARNING("curl plugin: `Regex' missing in `Match' block.");
      status = -1;
    }

    if (match->metric == NULL) {
      WARNING("curl plugin: `Metric' missing in `Match' block.");
      status = -1;
    }

    if (match->dstype == 0) {
      WARNING("curl plugin: `DSType' missing in `Match' block.");
      status = -1;
    }

    break;
  } /* while (status == 0) */

  if (status != 0) {
    cc_web_match_free(match);
    return status;
  }

  match->match =
      match_create_simple(match->regex, match->exclude_regex, match->dstype);
  if (match->match == NULL) {
    ERROR("curl plugin: match_create_simple failed.");
    cc_web_match_free(match);
    return -1;
  } else {
    web_match_t *prev;

    prev = page->matches;
    while ((prev != NULL) && (prev->next != NULL))
      prev = prev->next;

    if (prev == NULL)
      page->matches = match;
    else
      prev->next = match;
  }

  return 0;
} /* }}} int cc_config_add_match */

static int cc_page_init_curl(web_page_t *wp) /* {{{ */
{
  wp->curl = curl_easy_init();
  if (wp->curl == NULL) {
    ERROR("curl plugin: curl_easy_init failed.");
    return -1;
  }

  curl_easy_setopt(wp->curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(wp->curl, CURLOPT_WRITEFUNCTION, cc_curl_callback);
  curl_easy_setopt(wp->curl, CURLOPT_WRITEDATA, wp);
  curl_easy_setopt(wp->curl, CURLOPT_USERAGENT, COLLECTD_USERAGENT);
  curl_easy_setopt(wp->curl, CURLOPT_ERRORBUFFER, wp->curl_errbuf);
  curl_easy_setopt(wp->curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(wp->curl, CURLOPT_MAXREDIRS, 50L);
  curl_easy_setopt(wp->curl, CURLOPT_IPRESOLVE, wp->address_family);

  if (wp->user != NULL) {
#ifdef HAVE_CURLOPT_USERNAME
    curl_easy_setopt(wp->curl, CURLOPT_USERNAME, wp->user);
    curl_easy_setopt(wp->curl, CURLOPT_PASSWORD,
                     (wp->pass == NULL) ? "" : wp->pass);
#else
    size_t credentials_size;

    credentials_size = strlen(wp->user) + 2;
    if (wp->pass != NULL)
      credentials_size += strlen(wp->pass);

    wp->credentials = malloc(credentials_size);
    if (wp->credentials == NULL) {
      ERROR("curl plugin: malloc failed.");
      return -1;
    }

    snprintf(wp->credentials, credentials_size, "%s:%s", wp->user,
             (wp->pass == NULL) ? "" : wp->pass);
    curl_easy_setopt(wp->curl, CURLOPT_USERPWD, wp->credentials);
#endif

    if (wp->digest)
      curl_easy_setopt(wp->curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
  }

  curl_easy_setopt(wp->curl, CURLOPT_SSL_VERIFYPEER, (long)wp->verify_peer);
  curl_easy_setopt(wp->curl, CURLOPT_SSL_VERIFYHOST, wp->verify_host ? 2L : 0L);
  if (wp->cacert != NULL)
    curl_easy_setopt(wp->curl, CURLOPT_CAINFO, wp->cacert);
  if (wp->headers != NULL)
    curl_easy_setopt(wp->curl, CURLOPT_HTTPHEADER, wp->headers);
  if (wp->post_body != NULL)
    curl_easy_setopt(wp->curl, CURLOPT_POSTFIELDS, wp->post_body);

#ifdef HAVE_CURLOPT_TIMEOUT_MS
  if (wp->timeout >= 0)
    curl_easy_setopt(wp->curl, CURLOPT_TIMEOUT_MS, (long)wp->timeout);
  else
    curl_easy_setopt(wp->curl, CURLOPT_TIMEOUT_MS,
                     (long)CDTIME_T_TO_MS(plugin_get_interval()));
#endif

  return 0;
} /* }}} int cc_page_init_curl */

static int cc_config_add_page(oconfig_item_t *ci) /* {{{ */
{
  cdtime_t interval = 0;
  web_page_t *page;
  int status;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)) {
    WARNING("curl plugin: `Page' blocks need exactly one string argument.");
    return -1;
  }

  page = calloc(1, sizeof(*page));
  if (page == NULL) {
    ERROR("curl plugin: calloc failed.");
    return -1;
  }
  page->address_family = CURL_IPRESOLVE_WHATEVER;
  page->digest = false;
  page->verify_peer = true;
  page->verify_host = true;
  page->response_time = false;
  page->response_code = false;
  page->timeout = -1;

  page->instance = strdup(ci->values[0].value.string);
  if (page->instance == NULL) {
    ERROR("curl plugin: strdup failed.");
    sfree(page);
    return -1;
  }

  /* Process all children */
  status = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("MetricPrefix", child->key) == 0)
      status = cf_util_get_string(child, &page->metric_prefix);
    else if (strcasecmp("Label", child->key) == 0)
      status = cf_util_get_label(child, &page->labels);
    else if (strcasecmp("URL", child->key) == 0)
      status = cf_util_get_string(child, &page->url);
    else if (strcasecmp("AddressFamily", child->key) == 0) {
      char *af = NULL;
      status = cf_util_get_string(child, &af);
      if (status != 0 || af == NULL) {
        WARNING("curl plugin: Cannot parse value of `%s' "
                "for instance `%s'.",
                child->key, page->instance);
      } else if (strcasecmp("any", af) == 0) {
        page->address_family = CURL_IPRESOLVE_WHATEVER;
      } else if (strcasecmp("ipv4", af) == 0) {
        page->address_family = CURL_IPRESOLVE_V4;
      } else if (strcasecmp("ipv6", af) == 0) {
        /* If curl supports ipv6, use it. If not, log a warning and
         * fall back to default - don't set status to non-zero.
         */
        curl_version_info_data *curl_info = curl_version_info(CURLVERSION_NOW);
        if (curl_info->features & CURL_VERSION_IPV6)
          page->address_family = CURL_IPRESOLVE_V6;
        else
          WARNING("curl plugin: IPv6 not supported by this libCURL. "
                  "Using fallback `any'.");
      } else {
        WARNING("curl plugin: Unsupported value of `%s' "
                "for instance `%s'.",
                child->key, page->instance);
        status = -1;
      }
      free(af);
    } else if (strcasecmp("User", child->key) == 0)
      status = cf_util_get_string(child, &page->user);
    else if (strcasecmp("Password", child->key) == 0)
      status = cf_util_get_string(child, &page->pass);
    else if (strcasecmp("Digest", child->key) == 0)
      status = cf_util_get_boolean(child, &page->digest);
    else if (strcasecmp("VerifyPeer", child->key) == 0)
      status = cf_util_get_boolean(child, &page->verify_peer);
    else if (strcasecmp("VerifyHost", child->key) == 0)
      status = cf_util_get_boolean(child, &page->verify_host);
    else if (strcasecmp("MeasureResponseTime", child->key) == 0)
      status = cf_util_get_boolean(child, &page->response_time);
    else if (strcasecmp("MeasureResponseCode", child->key) == 0)
      status = cf_util_get_boolean(child, &page->response_code);
    else if (strcasecmp("CACert", child->key) == 0)
      status = cf_util_get_string(child, &page->cacert);
    else if (strcasecmp("Match", child->key) == 0)
      /* Be liberal with failing matches => don't set `status'. */
      cc_config_add_match(page, child);
    else if (strcasecmp("Header", child->key) == 0)
      status = cc_config_append_string("Header", &page->headers, child);
    else if (strcasecmp("Post", child->key) == 0)
      status = cf_util_get_string(child, &page->post_body);
    else if (strcasecmp("Interval", child->key) == 0)
      status = cf_util_get_cdtime(child, &interval);
    else if (strcasecmp("Timeout", child->key) == 0)
      status = cf_util_get_int(child, &page->timeout);
    else if (strcasecmp("Statistics", child->key) == 0) {
      page->stats = curl_stats_from_config(child);
      if (page->stats == NULL)
        status = -1;
    } else {
      WARNING("curl plugin: Option `%s' not allowed here.", child->key);
      status = -1;
    }

    if (status != 0)
      break;
  } /* for (i = 0; i < ci->children_num; i++) */

  /* Additionial sanity checks and libCURL initialization. */
  while (status == 0) {
    if (page->url == NULL) {
      WARNING("curl plugin: `URL' missing in `Page' block.");
      status = -1;
    }

    if (page->matches == NULL && page->stats == NULL && !page->response_time &&
        !page->response_code) {
      assert(page->instance != NULL);
      WARNING("curl plugin: No (valid) `Match' block "
              "or Statistics or MeasureResponseTime or MeasureResponseCode "
              "within `Page' block `%s'.",
              page->instance);
      status = -1;
    }

    if (page->response_time) {
      if (page->metric_prefix == NULL)
        page->metric_response_time = strdup("curl_response_time_seconds");
      else
        page->metric_response_time = ssnprintf_alloc("%s__response_time_seconds", page->metric_prefix);

      if (page->metric_response_time == NULL) {
        ERROR("curl plugin: alloc metric response time string failed.");
        status = -1;
      }
    }
    if (page->response_code) {
      if (page->metric_prefix == NULL)
        page->metric_response_code = strdup("curl_response_code");
      else
        page->metric_response_code = ssnprintf_alloc("%s__response_code", page->metric_prefix);

      if (page->metric_response_code == NULL) {
        ERROR("curl plugin: alloc metric response code string failed.");
        status = -1;
      }
    }

    if (status == 0)
      status = cc_page_init_curl(page);

    break;
  } /* while (status == 0) */

  if (status != 0) {
    cc_web_page_free(page);
    return status;
  }

  if (page->matches != NULL) {
    web_match_t *wm = page->matches;
    while (wm != NULL) {
      if (page->metric_prefix != NULL) {
        char *metric = ssnprintf_alloc("%s%s", page->metric_prefix, wm->metric);
        if (metric == NULL) {
          ERROR("curl plugin: alloc metric string failed.");
          cc_web_page_free(page);
          return -1;
        }
        sfree(wm->metric);
        wm->metric = metric;
      }

      if (page->instance != NULL)
        metric_label_set(&wm->tmpl, "instance", page->instance);

      for (size_t i = 0; i < page->labels.num; i++)
        metric_label_set(&wm->tmpl, page->labels.ptr[i].name, page->labels.ptr[i].value);

      for (size_t i = 0; i < wm->labels.num; i++)
        metric_label_set(&wm->tmpl, wm->labels.ptr[i].name, wm->labels.ptr[i].value);

      wm = wm->next;
    }
  }

  if (page->response_time || (page->stats != NULL) || page->response_code) {
    if (page->instance != NULL)
      metric_label_set(&page->tmpl, "instance", page->instance);

    for (size_t i = 0; i < page->labels.num; i++)
      metric_label_set(&page->tmpl, page->labels.ptr[i].name, page->labels.ptr[i].value);
  }

  /* If all went well, register this page for reading */
  char *cb_name = ssnprintf_alloc("curl-%s-%s", page->instance, page->url);

  plugin_register_complex_read(/* group = */ NULL, cb_name, cc_read_page,
                               interval,
                               &(user_data_t){
                                   .data = page,
                                   .free_func = cc_web_page_free,
                               });
  sfree(cb_name);

  return 0;
} /* }}} int cc_config_add_page */

static int cc_config(oconfig_item_t *ci) /* {{{ */
{
  int success;
  int errors;
  int status;

  success = 0;
  errors = 0;

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Page", child->key) == 0) {
      status = cc_config_add_page(child);
      if (status == 0)
        success++;
      else
        errors++;
    } else {
      WARNING("curl plugin: Option `%s' not allowed here.", child->key);
      errors++;
    }
  }

  if ((success == 0) && (errors > 0)) {
    ERROR("curl plugin: All statements failed.");
    return -1;
  }

  return 0;
} /* }}} int cc_config */

static int cc_init(void) /* {{{ */
{
  curl_global_init(CURL_GLOBAL_SSL);
  return 0;
} /* }}} int cc_init */

static void cc_submit(const web_page_t *wp, const web_match_t *wm, /* {{{ */
                      value_t value) {
  metric_family_t fam = {0};
  value_t mvalue;

  if (wm->dstype & UTILS_MATCH_DS_TYPE_GAUGE) {
    fam.type = METRIC_TYPE_GAUGE;
    mvalue.gauge = value.gauge;
  } else if (wm->dstype & UTILS_MATCH_DS_TYPE_COUNTER) {
    fam.type = METRIC_TYPE_COUNTER;
    mvalue.counter = value.counter;
  } else if (wm->dstype & UTILS_MATCH_DS_TYPE_DERIVE) {
    fam.type = METRIC_TYPE_COUNTER;
    mvalue.counter = value.derive;
  } else {
    return;
  }

  fam.name = wm->metric;
  fam.help = wm->help;

  metric_family_append(&fam, NULL,NULL, mvalue, &wm->tmpl);

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0) {
    ERROR("curl plugin: plugin_dispatch_metric_family failed: %s", STRERROR(status));
  }
  metric_family_metric_reset(&fam);
} /* }}} void cc_submit */

static void cc_submit_gauge(char *metric, gauge_t value, metric_t *tmpl) /* {{{ */
{
  metric_family_t fam = {
   .name = metric,
   .type = METRIC_TYPE_GAUGE,
  };

  metric_family_append(&fam, NULL, NULL, (value_t){.gauge = value}, tmpl);

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0) {
    ERROR("curl plugin: plugin_dispatch_metric_family failed: %s", STRERROR(status));
  }
  metric_family_metric_reset(&fam);
} /* }}} void cc_submit_gauge */

static int cc_read_page(user_data_t *ud) /* {{{ */
{

  if ((ud == NULL) || (ud->data == NULL)) {
    ERROR("curl plugin: cc_read_page: Invalid user data.");
    return -1;
  }

  web_page_t *wp = (web_page_t *)ud->data;

  int status;
  cdtime_t start = 0;

  if (wp->response_time)
    start = cdtime();

  wp->buffer_fill = 0;

  curl_easy_setopt(wp->curl, CURLOPT_URL, wp->url);

  status = curl_easy_perform(wp->curl);
  if (status != CURLE_OK) {
    ERROR("curl plugin: curl_easy_perform failed with status %i: %s", status,
          wp->curl_errbuf);
    return -1;
  }

  if (wp->response_time)
    cc_submit_gauge(wp->metric_response_time, CDTIME_T_TO_DOUBLE(cdtime() - start), &wp->tmpl);

  if (wp->stats != NULL)
    curl_stats_dispatch(wp->stats, wp->curl, &wp->tmpl);

  if (wp->response_code) {
    long response_code = 0;
    status = curl_easy_getinfo(wp->curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (status != CURLE_OK) {
      ERROR("curl plugin: Fetching response code failed with status %i: %s",
            status, wp->curl_errbuf);
    } else {
      cc_submit_gauge(wp->metric_response_code, (gauge_t) response_code, &wp->tmpl);
    }
  }

  for (web_match_t *wm = wp->matches; wm != NULL; wm = wm->next) {
    cu_match_value_t *mv;

    status = match_apply(wm->match, wp->buffer);
    if (status != 0) {
      WARNING("curl plugin: match_apply failed.");
      continue;
    }

    mv = match_get_user_data(wm->match);
    if (mv == NULL) {
      WARNING("curl plugin: match_get_user_data returned NULL.");
      continue;
    }

    cc_submit(wp, wm, mv->value);
    match_value_reset(mv);
  } /* for (wm = wp->matches; wm != NULL; wm = wm->next) */

  return 0;
} /* }}} int cc_read_page */

void module_register(void) {
  plugin_register_complex_config("curl", cc_config);
  plugin_register_init("curl", cc_init);
} /* void module_register */
