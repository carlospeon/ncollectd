/**
 * collectd - src/apache.c
 * Copyright (C) 2006-2010  Florian octo Forster
 * Copyright (C) 2007       Florent EppO Monbillard
 * Copyright (C) 2009       Amit Gupta
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
 *   Florent EppO Monbillard <eppo at darox.net>
 *   - connections/lighttpd extension
 *   Amit Gupta <amit.gupta221 at gmail.com>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <curl/curl.h>

enum server_enum { APACHE = 0, LIGHTTPD };

struct apache_s {
  int server_type;
  char *name;
  char *url;
  char *user;
  char *pass;
  bool verify_peer;
  bool verify_host;
  char *cacert;
  char *ssl_ciphers;
  char *server; /* user specific server type */
  label_set_t labels;
  char *apache_buffer;
  char apache_curl_error[CURL_ERROR_SIZE];
  size_t apache_buffer_size;
  size_t apache_buffer_fill;
  int timeout;
  CURL *curl;
};

typedef struct apache_s apache_t;

enum {
  FAM_APACHE_REQUESTS,
  FAM_APACHE_BYTES,
  FAM_APACHE_WORKERS,
  FAM_APACHE_SCOREBOARD,
  FAM_APACHE_CONNECTIONS,
  FAM_APACHE_PROCESSES,
  FAM_APACHE_UPTIME,
  FAM_APACHE_UP,
  FAM_APACHE_MAX
};

/* TODO: Remove this prototype */
static int apache_read_host(user_data_t *user_data);

static void apache_free(void *arg)
{
  apache_t *st = arg;

  if (st == NULL)
    return;

  sfree(st->name);
  sfree(st->url);
  sfree(st->user);
  sfree(st->pass);
  sfree(st->cacert);
  sfree(st->ssl_ciphers);
  sfree(st->server);
  sfree(st->apache_buffer);
  label_set_reset(&st->labels);
  if (st->curl) {
    curl_easy_cleanup(st->curl);
    st->curl = NULL;
  }
  sfree(st);
}

static size_t apache_curl_callback(void *buf, size_t size, size_t nmemb, void *user_data)
{
  apache_t *st = user_data;
  if (st == NULL) {
    ERROR("apache plugin: apache_curl_callback: user_data pointer is NULL.");
    return 0;
  }

  size_t len = size * nmemb;
  if (len == 0)
    return len;

  if ((st->apache_buffer_fill + len) >= st->apache_buffer_size) {
    char *temp = realloc(st->apache_buffer, st->apache_buffer_fill + len + 1);
    if (temp == NULL) {
      ERROR("apache plugin: realloc failed.");
      return 0;
    }
    st->apache_buffer = temp;
    st->apache_buffer_size = st->apache_buffer_fill + len + 1;
  }

  memcpy(st->apache_buffer + st->apache_buffer_fill, (char *)buf, len);
  st->apache_buffer_fill += len;
  st->apache_buffer[st->apache_buffer_fill] = 0;

  return len;
}

static size_t apache_header_callback(void *buf, size_t size, size_t nmemb, void *user_data)
{
  apache_t *st = user_data;
  if (st == NULL) {
    ERROR("apache plugin: apache_header_callback: user_data pointer is NULL.");
    return 0;
  }

  size_t len = size * nmemb;
  if (len == 0)
    return len;

  /* look for the Server header */
  if (strncasecmp(buf, "Server: ", strlen("Server: ")) != 0)
    return len;

  if (strstr(buf, "Apache") != NULL)
    st->server_type = APACHE;
  else if (strstr(buf, "lighttpd") != NULL)
    st->server_type = LIGHTTPD;
  else if (strstr(buf, "IBM_HTTP_Server") != NULL)
    st->server_type = APACHE;
  else {
    const char *hdr = buf;

    hdr += strlen("Server: ");
    NOTICE("apache plugin: Unknown server software: %s", hdr);
  }

  return len;
}

/* Configuration handling functiions
 * <Plugin apache>
 *   <Instance "instance_name">
 *     URL ...
 *   </Instance>
 *   URL ...
 * </Plugin>
 */
static int config_add(oconfig_item_t *ci)
{
  apache_t *st = calloc(1, sizeof(*st));
  if (st == NULL) {
    ERROR("apache plugin: calloc failed.");
    return -1;
  }

  st->timeout = -1;

  int status = cf_util_get_string(ci, &st->name);
  if (status != 0) {
    sfree(st);
    return status;
  }
  assert(st->name != NULL);

  cdtime_t interval = 0;
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("URL", child->key) == 0)
      status = cf_util_get_string(child, &st->url);
    else if (strcasecmp("User", child->key) == 0)
      status = cf_util_get_string(child, &st->user);
    else if (strcasecmp("Password", child->key) == 0)
      status = cf_util_get_string(child, &st->pass);
    else if (strcasecmp("VerifyPeer", child->key) == 0)
      status = cf_util_get_boolean(child, &st->verify_peer);
    else if (strcasecmp("VerifyHost", child->key) == 0)
      status = cf_util_get_boolean(child, &st->verify_host);
    else if (strcasecmp("CACert", child->key) == 0)
      status = cf_util_get_string(child, &st->cacert);
    else if (strcasecmp("SSLCiphers", child->key) == 0)
      status = cf_util_get_string(child, &st->ssl_ciphers);
    else if (strcasecmp("Server", child->key) == 0)
      status = cf_util_get_string(child, &st->server);
    else if (strcasecmp("Timeout", child->key) == 0)
      status = cf_util_get_int(child, &st->timeout);
    else if (strcasecmp("Label", child->key) == 0)
      status = cf_util_get_label(child, &st->labels);
    else if (strcasecmp("Interval", child->key) == 0)
      status = cf_util_get_cdtime(child, &interval);
    else {
      WARNING("apache plugin: Option `%s' not allowed here.", child->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  /* Check if struct is complete.. */
  if ((status == 0) && (st->url == NULL)) {
    ERROR("apache plugin: Instance `%s': "
          "No URL has been configured.",
          st->name);
    status = -1;
  }

  if (status != 0) {
    apache_free(st);
    return -1;
  }

  char callback_name[3 * DATA_MAX_NAME_LEN];

  snprintf(callback_name, sizeof(callback_name), "apache/%s",
           (st->name != NULL) ? st->name : "default");

  return plugin_register_complex_read(
      /* group = */ NULL,
      /* name      = */ callback_name,
      /* callback  = */ apache_read_host,
      /* interval  = */ interval,
      &(user_data_t){
          .data = st,
          .free_func = apache_free,
      });
}

static int config(oconfig_item_t *ci)
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("Instance", child->key) == 0)
      config_add(child);
    else
      WARNING("apache plugin: The configuration option "
              "\"%s\" is not allowed here. Did you "
              "forget to add an <Instance /> block "
              "around the configuration?",
              child->key);
  }

  return 0;
}
/* initialize curl for each host */
static int init_host(apache_t *st)
{
  assert(st->url != NULL);
  /* (Assured by `config_add') */

  if (st->curl != NULL) {
    curl_easy_cleanup(st->curl);
    st->curl = NULL;
  }

  if ((st->curl = curl_easy_init()) == NULL) {
    ERROR("apache plugin: init_host: `curl_easy_init' failed.");
    return -1;
  }

  curl_easy_setopt(st->curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(st->curl, CURLOPT_WRITEFUNCTION, apache_curl_callback);
  curl_easy_setopt(st->curl, CURLOPT_WRITEDATA, st);

  /* not set as yet if the user specified string doesn't match apache or
   * lighttpd, then ignore it. Headers will be parsed to find out the
   * server type */
  st->server_type = -1;

  if (st->server != NULL) {
    if (strcasecmp(st->server, "apache") == 0)
      st->server_type = APACHE;
    else if (strcasecmp(st->server, "lighttpd") == 0)
      st->server_type = LIGHTTPD;
    else if (strcasecmp(st->server, "ibm_http_server") == 0)
      st->server_type = APACHE;
    else
      WARNING("apache plugin: Unknown `Server' setting: %s", st->server);
  }

  /* if not found register a header callback to determine the server_type */
  if (st->server_type == -1) {
    curl_easy_setopt(st->curl, CURLOPT_HEADERFUNCTION, apache_header_callback);
    curl_easy_setopt(st->curl, CURLOPT_WRITEHEADER, st);
  }

  curl_easy_setopt(st->curl, CURLOPT_USERAGENT, COLLECTD_USERAGENT);
  curl_easy_setopt(st->curl, CURLOPT_ERRORBUFFER, st->apache_curl_error);

  if (st->user != NULL) {
#ifdef HAVE_CURLOPT_USERNAME
    curl_easy_setopt(st->curl, CURLOPT_USERNAME, st->user);
    curl_easy_setopt(st->curl, CURLOPT_PASSWORD,
                     (st->pass == NULL) ? "" : st->pass);
#else
    static char credentials[1024];
    int status = snprintf(credentials, sizeof(credentials), "%s:%s", st->user,
                          (st->pass == NULL) ? "" : st->pass);
    if ((status < 0) || ((size_t)status >= sizeof(credentials))) {
      ERROR("apache plugin: init_host: Returning an error "
            "because the credentials have been "
            "truncated.");
      curl_easy_cleanup(st->curl);
      st->curl = NULL;
      return -1;
    }

    curl_easy_setopt(st->curl, CURLOPT_USERPWD, credentials);
#endif
  }

  curl_easy_setopt(st->curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(st->curl, CURLOPT_MAXREDIRS, 50L);

  curl_easy_setopt(st->curl, CURLOPT_SSL_VERIFYPEER, (long)st->verify_peer);
  curl_easy_setopt(st->curl, CURLOPT_SSL_VERIFYHOST, st->verify_host ? 2L : 0L);
  if (st->cacert != NULL)
    curl_easy_setopt(st->curl, CURLOPT_CAINFO, st->cacert);
  if (st->ssl_ciphers != NULL)
    curl_easy_setopt(st->curl, CURLOPT_SSL_CIPHER_LIST, st->ssl_ciphers);

#ifdef HAVE_CURLOPT_TIMEOUT_MS
  if (st->timeout >= 0)
    curl_easy_setopt(st->curl, CURLOPT_TIMEOUT_MS, (long)st->timeout);
  else
    curl_easy_setopt(st->curl, CURLOPT_TIMEOUT_MS,
                     (long)CDTIME_T_TO_MS(plugin_get_interval()));
#endif

  return 0;
}

static void submit_scoreboard(char *buf, apache_t *st,
                              metric_family_t *fam_scoreboard, metric_t *tmpl)
{
  /*
   * Scoreboard Key:
   * "_" Waiting for Connection, "S" Starting up,
   * "R" Reading Request for apache and read-POST for lighttpd,
   * "W" Sending Reply, "K" Keepalive (read), "D" DNS Lookup,
   * "C" Closing connection, "L" Logging, "G" Gracefully finishing,
   * "I" Idle cleanup of worker, "." Open slot with no current process
   * Lighttpd specific legends -
   * "E" hard error, "." connect, "h" handle-request,
   * "q" request-start, "Q" request-end, "s" response-start
   * "S" response-end, "r" read
   */
  long long open = 0LL;
  long long waiting = 0LL;
  long long starting = 0LL;
  long long reading = 0LL;
  long long sending = 0LL;
  long long keepalive = 0LL;
  long long dnslookup = 0LL;
  long long closing = 0LL;
  long long logging = 0LL;
  long long finishing = 0LL;
  long long idle_cleanup = 0LL;

  /* lighttpd specific */
  long long hard_error = 0LL;
  long long lighttpd_read = 0LL;
  long long handle_request = 0LL;
  long long request_start = 0LL;
  long long request_end = 0LL;
  long long response_start = 0LL;
  long long response_end = 0LL;

  for (int i = 0; buf[i] != '\0'; i++) {
    if (buf[i] == '.')
      open++;
    else if (buf[i] == '_')
      waiting++;
    else if (buf[i] == 'S')
      starting++;
    else if (buf[i] == 'R')
      reading++;
    else if (buf[i] == 'W')
      sending++;
    else if (buf[i] == 'K')
      keepalive++;
    else if (buf[i] == 'D')
      dnslookup++;
    else if (buf[i] == 'C')
      closing++;
    else if (buf[i] == 'L')
      logging++;
    else if (buf[i] == 'G')
      finishing++;
    else if (buf[i] == 'I')
      idle_cleanup++;
    else if (buf[i] == 'r')
      lighttpd_read++;
    else if (buf[i] == 'h')
      handle_request++;
    else if (buf[i] == 'E')
      hard_error++;
    else if (buf[i] == 'q')
      request_start++;
    else if (buf[i] == 'Q')
      request_end++;
    else if (buf[i] == 's')
      response_start++;
    else if (buf[i] == 'S')
      response_end++;
  }

  if (st->server_type == APACHE) {
    metric_family_append(fam_scoreboard, "state", "open",
                         (value_t){.gauge = open}, tmpl);
    metric_family_append(fam_scoreboard, "state", "waiting",
                         (value_t){.gauge = waiting}, tmpl);
    metric_family_append(fam_scoreboard, "state", "starting",
                         (value_t){.gauge = starting}, tmpl);
    metric_family_append(fam_scoreboard, "state", "reading",
                         (value_t){.gauge = reading}, tmpl);
    metric_family_append(fam_scoreboard, "state", "sending",
                         (value_t){.gauge = sending}, tmpl);
    metric_family_append(fam_scoreboard, "state", "keepalive",
                         (value_t){.gauge = keepalive}, tmpl);
    metric_family_append(fam_scoreboard, "state", "dnslookup",
                         (value_t){.gauge = dnslookup}, tmpl);
    metric_family_append(fam_scoreboard, "state", "closing",
                         (value_t){.gauge = closing}, tmpl);
    metric_family_append(fam_scoreboard, "state", "logging",
                         (value_t){.gauge = logging}, tmpl);
    metric_family_append(fam_scoreboard, "state", "finishing",
                         (value_t){.gauge = finishing}, tmpl);
    metric_family_append(fam_scoreboard, "state", "idle_cleanup",
                         (value_t){.gauge = idle_cleanup}, tmpl);
  } else {
    metric_family_append(fam_scoreboard, "state", "connect",
                         (value_t){.gauge = open}, tmpl);
    metric_family_append(fam_scoreboard, "state", "close",
                         (value_t){.gauge = closing}, tmpl);
    metric_family_append(fam_scoreboard, "state", "hard_error",
                         (value_t){.gauge = hard_error}, tmpl);
    metric_family_append(fam_scoreboard, "state", "read",
                         (value_t){.gauge = lighttpd_read}, tmpl);
    metric_family_append(fam_scoreboard, "state", "read_post",
                         (value_t){.gauge = reading}, tmpl);
    metric_family_append(fam_scoreboard, "state", "write",
                         (value_t){.gauge = sending}, tmpl);
    metric_family_append(fam_scoreboard, "state", "handle_request",
                         (value_t){.gauge = handle_request}, tmpl);
    metric_family_append(fam_scoreboard, "state", "request_start",
                         (value_t){.gauge = request_start}, tmpl);
    metric_family_append(fam_scoreboard, "state", "request_end",
                         (value_t){.gauge = request_end}, tmpl);
    metric_family_append(fam_scoreboard, "state", "response_start",
                         (value_t){.gauge = response_start}, tmpl);
    metric_family_append(fam_scoreboard, "state", "response_end",
                         (value_t){.gauge = response_end}, tmpl);
  }
}

static int apache_read_host(user_data_t *user_data)
{
  metric_family_t fams[FAM_APACHE_MAX] = {
   [FAM_APACHE_REQUESTS] = {
     .name = "apache_requests_total",
     .type = METRIC_TYPE_COUNTER,
     .help = "Apache total requests",
   },
   [FAM_APACHE_BYTES] = {
     .name = "apache_bytes_total",
     .type = METRIC_TYPE_COUNTER,
     .help = "Apache total kbytes sent",
   },
   [FAM_APACHE_WORKERS] = {
     .name = "apache_workers",
     .type = METRIC_TYPE_GAUGE,
     .help = "Apache workers",
   },
   [FAM_APACHE_SCOREBOARD] = {
     .name = "apache_scoreboard",
     .type = METRIC_TYPE_GAUGE,
     .help = "Apache scoreboard statuses",
   },
   [FAM_APACHE_CONNECTIONS] = {
     .name = "apache_connections",
     .type = METRIC_TYPE_GAUGE,
     .help = "Apache connections",
   },
   [FAM_APACHE_PROCESSES] = {
     .name = "apache_processes",
     .type = METRIC_TYPE_GAUGE,
     .help = "Apache processes",
   },
   [FAM_APACHE_UPTIME] = {
     .name = "apache_uptime_seconds",
     .type = METRIC_TYPE_GAUGE,
     .help = "Apache server uptime",
   },
   [FAM_APACHE_UP] = {
     .name = "apache_up",
     .type = METRIC_TYPE_GAUGE,
     .help = "Could the apache server be reached",
   },
  };

  apache_t *st = user_data->data;

  assert(st->url != NULL);
  /* (Assured by `config_add') */

  if (st->curl == NULL) {
    if (init_host(st) != 0)
      return -1;
  }
  assert(st->curl != NULL);

  st->apache_buffer_fill = 0;

  metric_t tmpl = {0};
  if (st->name != NULL)
    metric_label_set(&tmpl, "instance", st->name);

  for (size_t i = 0; i < st->labels.num; i++) {
    metric_label_set(&tmpl, st->labels.ptr[i].name, st->labels.ptr[i].value);
  }

  curl_easy_setopt(st->curl, CURLOPT_URL, st->url);

  if (curl_easy_perform(st->curl) != CURLE_OK) {
    ERROR("apache: curl_easy_perform failed: %s", st->apache_curl_error);

    tmpl.value.gauge = 0;
    metric_family_metric_append(&fams[FAM_APACHE_UP], tmpl);
    int status = plugin_dispatch_metric_family(&fams[FAM_APACHE_UP]);
    if (status != 0)
      ERROR("apache plugin: plugin_dispatch_metric_family failed: %s", STRERROR(status));
    metric_reset(&tmpl);

    return -1;
  }

  /* fallback - server_type to apache if not set at this time */
  if (st->server_type == -1) {
    WARNING("apache plugin: Unable to determine server software "
            "automatically. Will assume Apache.");
    st->server_type = APACHE;
  }

  char *content_type;
  static const char *text_plain = "text/plain";
  int status =
      curl_easy_getinfo(st->curl, CURLINFO_CONTENT_TYPE, &content_type);
  if ((status == CURLE_OK) && (content_type != NULL) &&
      (strncasecmp(content_type, text_plain, strlen(text_plain)) != 0)) {
    WARNING("apache plugin: `Content-Type' response header is not `%s' "
            "(received: `%s'). Expecting unparseable data. Please check `URL' "
            "parameter (missing `?auto' suffix ?)",
            text_plain, content_type);
  }

  char *ptr = st->apache_buffer;
  char *saveptr = NULL;
  char *line;
  /* Apache http mod_status added a second set of BusyWorkers, IdleWorkers in
   * https://github.com/apache/httpd/commit/6befc18
   * For Apache 2.4.35 and up we need to ensure only one key is used.
   * S.a. https://bz.apache.org/bugzilla/show_bug.cgi?id=63300
   */
  int apache_connections_submitted = 0, apache_idle_workers_submitted = 0;

  while ((line = strtok_r(ptr, "\n\r", &saveptr)) != NULL) {
    ptr = NULL;
    char *fields[4];

    int fields_num = strsplit(line, fields, STATIC_ARRAY_SIZE(fields));

    if (fields_num == 3) {
      if ((strcmp(fields[0], "Total") == 0) &&
          (strcmp(fields[1], "Accesses:") == 0)) {
        tmpl.value.counter = atoll(fields[2]);
        metric_family_metric_append(&fams[FAM_APACHE_REQUESTS], tmpl);
      } else if ((strcmp(fields[0], "Total") == 0) &&
                 (strcmp(fields[1], "kBytes:") == 0)) {
        tmpl.value.counter = 1024LL * atoll(fields[2]);
        metric_family_metric_append(&fams[FAM_APACHE_BYTES], tmpl);
      }
    } else if (fields_num == 2) {
      if (strcmp(fields[0], "Scoreboard:") == 0) {
        submit_scoreboard(fields[1], st, &fams[FAM_APACHE_SCOREBOARD], &tmpl);
      } else if (!apache_connections_submitted &&
               ((strcmp(fields[0], "BusyServers:") == 0) /* Apache 1.* */
                || (strcmp(fields[0], "BusyWorkers:") == 0)) /* Apache 2.* */) {
        metric_family_append(&fams[FAM_APACHE_WORKERS], "state", "busy",
                             (value_t){.gauge = atol(fields[1])}, &tmpl);
        apache_connections_submitted++;
      } else if (!apache_idle_workers_submitted &&
                 ((strcmp(fields[0], "IdleServers:") == 0) /* Apache 1.x */
                  ||(strcmp(fields[0], "IdleWorkers:") == 0)) /* Apache 2.x */) {
        metric_family_append(&fams[FAM_APACHE_WORKERS], "state", "idle",
                             (value_t){.gauge = atol(fields[1])}, &tmpl);
        apache_idle_workers_submitted++;
      } else if (strcmp(fields[0], "ConnsTotal:") == 0) {
        metric_family_append(&fams[FAM_APACHE_CONNECTIONS], "state", "total",
                             (value_t){.gauge = atol(fields[1])}, &tmpl);
      } else if (strcmp(fields[0], "ConnsAsyncWriting:") == 0) {
        metric_family_append(&fams[FAM_APACHE_CONNECTIONS], "state", "writting",
                             (value_t){.gauge = atol(fields[1])}, &tmpl);
      } else if (strcmp(fields[0], "ConnsAsyncKeepAlive:") == 0) {
        metric_family_append(&fams[FAM_APACHE_CONNECTIONS], "state", "keepalive",
                             (value_t){.gauge = atol(fields[1])}, &tmpl);
      } else if (strcmp(fields[0], "ConnsAsyncClosing:") == 0) {
        metric_family_append(&fams[FAM_APACHE_CONNECTIONS], "state", "closing",
                             (value_t){.gauge = atol(fields[1])}, &tmpl);
      } else if (strcmp(fields[0], "Processes:") == 0) {
        tmpl.value.gauge = atol(fields[1]);
        metric_family_metric_append(&fams[FAM_APACHE_PROCESSES], tmpl);
      } else if (strcmp(fields[0], "ServerUptimeSeconds:") == 0) {
        tmpl.value.gauge = atol(fields[1]);
        metric_family_metric_append(&fams[FAM_APACHE_UPTIME], tmpl);
      }

    }
  }

  metric_reset(&tmpl);

  st->apache_buffer_fill = 0;

  for (size_t i = 0; i < FAM_APACHE_MAX; i++) {
    if (fams[i].metric.num > 0) {
      int status = plugin_dispatch_metric_family(&fams[i]);
      if (status != 0) {
        ERROR("apache plugin: plugin_dispatch_metric_family failed: %s",
              STRERROR(status));
      }
      metric_family_metric_reset(&fams[i]);
    }
  }

  return 0;
}

static int apache_init(void)
{
  /* Call this while collectd is still single-threaded to avoid
   * initialization issues in libgcrypt. */
  curl_global_init(CURL_GLOBAL_SSL);
  return 0;
}

void module_register(void)
{
  plugin_register_complex_config("apache", config);
  plugin_register_init("apache", apache_init);
}
