/**
 * collectd - src/ping.c
 * Copyright (C) 2005-2012  Florian octo Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils_complain.h"

#include <netinet/in.h>
#if HAVE_NETDB_H
#include <netdb.h> /* NI_MAXHOST */
#endif

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#include <oping.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#if defined(OPING_VERSION) && (OPING_VERSION >= 1003000)
#define HAVE_OPING_1_3
#endif

/*
 * Private data types
 */
struct hostlist_s {
  char *host;

  uint32_t pkg_sent;
  uint32_t pkg_recv;
  uint32_t pkg_missed;

  distribution_t *dist_latency;

  struct hostlist_s *next;
};
typedef struct hostlist_s hostlist_t;

/*
 * Private variables
 */
static hostlist_t *hostlist_head;

static int ping_af = PING_DEF_AF;
static char *ping_source;
#ifdef HAVE_OPING_1_3
static char *ping_device;
#endif
static char *ping_data;
static int ping_ttl = PING_DEF_TTL;
static double ping_interval = 1.0;
static double ping_timeout = 0.9;
static int ping_max_missed = -1;
static size_t num_buckets = 100;

static pthread_mutex_t ping_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ping_cond = PTHREAD_COND_INITIALIZER;
static int ping_thread_loop;
static int ping_thread_error;
static pthread_t ping_thread_id;

static const char *config_keys[] = {"Host",    "SourceAddress", "AddressFamily",
#ifdef HAVE_OPING_1_3
                                    "Device",
#endif
                                    "Size",    "TTL",           "Interval",
                                    "Timeout", "MaxMissed"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

/*
 * Private functions
 */
/* Assure that `ts->tv_nsec' is in the range 0 .. 999999999 */
static void time_normalize(struct timespec *ts)
{
  while (ts->tv_nsec < 0) {
    if (ts->tv_sec == 0) {
      ts->tv_nsec = 0;
      return;
    }

    ts->tv_sec -= 1;
    ts->tv_nsec += 1000000000;
  }

  while (ts->tv_nsec >= 1000000000) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000;
  }
}

/* Add `ts_int' to `tv_begin' and store the result in `ts_dest'. If the result
 * is larger than `tv_end', copy `tv_end' to `ts_dest' instead. */
static void time_calc(struct timespec *ts_dest,
                      const struct timespec *ts_int,
                      const struct timeval *tv_begin,
                      const struct timeval *tv_end)
{
  ts_dest->tv_sec = tv_begin->tv_sec + ts_int->tv_sec;
  ts_dest->tv_nsec = (tv_begin->tv_usec * 1000) + ts_int->tv_nsec;
  time_normalize(ts_dest);

  /* Assure that `(begin + interval) > end'.
   * This may seem overly complicated, but `tv_sec' is of type `time_t'
   * which may be `unsigned. *sigh* */
  if ((tv_end->tv_sec > ts_dest->tv_sec) ||
      ((tv_end->tv_sec == ts_dest->tv_sec) &&
       ((tv_end->tv_usec * 1000) > ts_dest->tv_nsec))) {
    ts_dest->tv_sec = tv_end->tv_sec;
    ts_dest->tv_nsec = 1000 * tv_end->tv_usec;
  }

  time_normalize(ts_dest);
}

static int ping_dispatch_all(pingobj_t *pingobj)
{
  hostlist_t *hl;
  int status;

  for (pingobj_iter_t *iter = ping_iterator_get(pingobj); iter != NULL;
       iter = ping_iterator_next(iter)) { /* {{{ */
    char userhost[NI_MAXHOST];
    double latency;
    size_t param_size;

    param_size = sizeof(userhost);
    status = ping_iterator_get_info(iter,
#ifdef PING_INFO_USERNAME
                                    PING_INFO_USERNAME,
#else
                                    PING_INFO_HOSTNAME,
#endif
                                    userhost, &param_size);
    if (status != 0) {
      WARNING("ping plugin: ping_iterator_get_info failed: %s",
              ping_get_error(pingobj));
      continue;
    }

    for (hl = hostlist_head; hl != NULL; hl = hl->next)
      if (strcmp(userhost, hl->host) == 0)
        break;

    if (hl == NULL) {
      WARNING("ping plugin: Cannot find host %s.", userhost);
      continue;
    }

    param_size = sizeof(latency);
    status = ping_iterator_get_info(iter, PING_INFO_LATENCY, (void *)&latency,
                                    &param_size);
    if (status != 0) {
      WARNING("ping plugin: ping_iterator_get_info failed: %s",
              ping_get_error(pingobj));
      continue;
    }

    hl->pkg_sent++;
    if (latency >= 0.0) {
      hl->pkg_recv++;
      status = distribution_update(hl->dist_latency, latency);
      if (status != 0) {
        WARNING("ping plugin: distribution_update failed: %s",STRERROR(status));
      }
      /* reset missed packages counter */
      hl->pkg_missed = 0;
    } else
      hl->pkg_missed++;


    /* if the host did not answer our last N packages, trigger a resolv. */
    if ((ping_max_missed >= 0) &&
        (hl->pkg_missed >= ((uint32_t)ping_max_missed))) { /* {{{ */
      /* we reset the missed package counter here, since we only want to
       * trigger a resolv every N packages and not every package _AFTER_ N
       * missed packages */
      hl->pkg_missed = 0;

      WARNING("ping plugin: host %s has not answered %d PING requests,"
              " triggering resolve",
              hl->host, ping_max_missed);

      /* we trigger the resolv simply be removeing and adding the host to our
       * ping object */
      status = ping_host_remove(pingobj, hl->host);
      if (status != 0) {
        WARNING("ping plugin: ping_host_remove (%s) failed.", hl->host);
      } else {
        status = ping_host_add(pingobj, hl->host);
        if (status != 0)
          ERROR("ping plugin: ping_host_add (%s) failed.", hl->host);
      }
    } /* }}} ping_max_missed */
  }   /* }}} for (iter) */

  return 0;
}

static void *ping_thread(void *arg)
{
  struct timeval tv_begin;
  struct timeval tv_end;
  struct timespec ts_wait;
  struct timespec ts_int;

  int count;

  c_complain_t complaint = C_COMPLAIN_INIT_STATIC;

  pingobj_t *pingobj = ping_construct();
  if (pingobj == NULL) {
    ERROR("ping plugin: ping_construct failed.");
    pthread_mutex_lock(&ping_lock);
    ping_thread_error = 1;
    pthread_mutex_unlock(&ping_lock);
    return (void *)-1;
  }

  if (ping_af != PING_DEF_AF) {
    if (ping_setopt(pingobj, PING_OPT_AF, &ping_af) != 0)
      ERROR("ping plugin: Failed to set address family: %s",
            ping_get_error(pingobj));
  }

  if (ping_source != NULL)
    if (ping_setopt(pingobj, PING_OPT_SOURCE, (void *)ping_source) != 0)
      ERROR("ping plugin: Failed to set source address: %s",
            ping_get_error(pingobj));

#ifdef HAVE_OPING_1_3
  if (ping_device != NULL)
    if (ping_setopt(pingobj, PING_OPT_DEVICE, (void *)ping_device) != 0)
      ERROR("ping plugin: Failed to set device: %s", ping_get_error(pingobj));
#endif

  ping_setopt(pingobj, PING_OPT_TIMEOUT, (void *)&ping_timeout);
  ping_setopt(pingobj, PING_OPT_TTL, (void *)&ping_ttl);

  if (ping_data != NULL)
    ping_setopt(pingobj, PING_OPT_DATA, (void *)ping_data);

  /* Add all the hosts to the ping object. */
  count = 0;
  for (hostlist_t *hl = hostlist_head; hl != NULL; hl = hl->next) {
    int tmp_status;
    tmp_status = ping_host_add(pingobj, hl->host);
    if (tmp_status != 0)
      WARNING("ping plugin: ping_host_add (%s) failed: %s", hl->host,
              ping_get_error(pingobj));
    else
      count++;
  }

  if (count == 0) {
    ERROR("ping plugin: No host could be added to ping object. Giving up.");
    pthread_mutex_lock(&ping_lock);
    ping_thread_error = 1;
    pthread_mutex_unlock(&ping_lock);
    return (void *)-1;
  }

  /* Set up `ts_int' */
  {
    double temp_sec;
    double temp_nsec;

    temp_nsec = modf(ping_interval, &temp_sec);
    ts_int.tv_sec = (time_t)temp_sec;
    ts_int.tv_nsec = (long)(temp_nsec * 1000000000L);
  }

  pthread_mutex_lock(&ping_lock);
  while (ping_thread_loop > 0) {
    bool send_successful = false;

    if (gettimeofday(&tv_begin, NULL) < 0) {
      ERROR("ping plugin: gettimeofday failed: %s", STRERRNO);
      ping_thread_error = 1;
      break;
    }

    pthread_mutex_unlock(&ping_lock);

    int status = ping_send(pingobj);
    if (status < 0) {
      c_complain(LOG_ERR, &complaint, "ping plugin: ping_send failed: %s",
                 ping_get_error(pingobj));
    } else {
      c_release(LOG_NOTICE, &complaint, "ping plugin: ping_send succeeded.");
      send_successful = true;
    }

    pthread_mutex_lock(&ping_lock);

    if (ping_thread_loop <= 0)
      break;

    if (send_successful)
      (void)ping_dispatch_all(pingobj);

    if (gettimeofday(&tv_end, NULL) < 0) {
      ERROR("ping plugin: gettimeofday failed: %s", STRERRNO);
      ping_thread_error = 1;
      break;
    }

    /* Calculate the absolute time until which to wait and store it in
     * `ts_wait'. */
    time_calc(&ts_wait, &ts_int, &tv_begin, &tv_end);

    pthread_cond_timedwait(&ping_cond, &ping_lock, &ts_wait);
    if (ping_thread_loop <= 0)
      break;
  } /* while (ping_thread_loop > 0) */

  pthread_mutex_unlock(&ping_lock);
  ping_destroy(pingobj);

  return (void *)0;
}

static int start_thread(void)
{
  int status;

  pthread_mutex_lock(&ping_lock);

  if (ping_thread_loop != 0) {
    pthread_mutex_unlock(&ping_lock);
    return 0;
  }

  ping_thread_loop = 1;
  ping_thread_error = 0;
  status = plugin_thread_create(&ping_thread_id, ping_thread,
                                /* arg = */ (void *)0, "ping");
  if (status != 0) {
    ping_thread_loop = 0;
    ERROR("ping plugin: Starting thread failed.");
    pthread_mutex_unlock(&ping_lock);
    return -1;
  }

  pthread_mutex_unlock(&ping_lock);
  return 0;
}

static int stop_thread(void)
{
  int status;

  pthread_mutex_lock(&ping_lock);

  if (ping_thread_loop == 0) {
    pthread_mutex_unlock(&ping_lock);
    return -1;
  }

  ping_thread_loop = 0;
  pthread_cond_broadcast(&ping_cond);
  pthread_mutex_unlock(&ping_lock);

  status = pthread_join(ping_thread_id, /* return = */ NULL);
  if (status != 0) {
    ERROR("ping plugin: Stopping thread failed.");
    status = -1;
  }

  pthread_mutex_lock(&ping_lock);
  memset(&ping_thread_id, 0, sizeof(ping_thread_id));
  ping_thread_error = 0;
  pthread_mutex_unlock(&ping_lock);

  return status;
}

static int ping_init(void)
{
  if (hostlist_head == NULL) {
    NOTICE("ping plugin: No hosts have been configured.");
    return -1;
  }

  if (ping_timeout > ping_interval) {
    ping_timeout = 0.9 * ping_interval;
    WARNING("ping plugin: Timeout is greater than interval. "
            "Will use a timeout of %gs.",
            ping_timeout);
  }

#if defined(HAVE_SYS_CAPABILITY_H) && defined(CAP_NET_RAW)
  if (check_capability(CAP_NET_RAW) != 0) {
    if (getuid() == 0)
      WARNING("ping plugin: Running collectd as root, but the CAP_NET_RAW "
              "capability is missing. The plugin's read function will probably "
              "fail. Is your init system dropping capabilities?");
    else
      WARNING("ping plugin: collectd doesn't have the CAP_NET_RAW capability. "
              "If you don't want to run collectd as root, try running \"setcap "
              "cap_net_raw=ep\" on the collectd binary.");
  }
#endif

  return start_thread();
}

static int config_set_string(const char *name,
                             char **var, const char *value)
{
  char *tmp;

  tmp = strdup(value);
  if (tmp == NULL) {
    ERROR("ping plugin: Setting `%s' to `%s' failed: strdup failed: %s", name,
          value, STRERRNO);
    return 1;
  }

  if (*var != NULL)
    free(*var);
  *var = tmp;
  return 0;
}

static int ping_config(const char *key, const char *value)
{
  if (strcasecmp(key, "Host") == 0) {
    hostlist_t *hl;
    char *host;

    hl = malloc(sizeof(*hl));
    if (hl == NULL) {
      ERROR("ping plugin: malloc failed: %s", STRERRNO);
      return 1;
    }

    hl->dist_latency = distribution_new_linear(num_buckets, ping_timeout / num_buckets);
    if (hl->dist_latency == NULL) {
      sfree(hl);
      ERROR("ping plugin: Cannot create a distribution for latency");
      return 1;
    }

    host = strdup(value);
    if (host == NULL) {
      sfree(hl);
      distribution_destroy(hl->dist_latency);
      ERROR("ping plugin: strdup failed: %s", STRERRNO);
      return 1;
    }

    hl->host = host;
    hl->pkg_sent = 0;
    hl->pkg_recv = 0;
    hl->pkg_missed = 0;

    hl->next = hostlist_head;
    hostlist_head = hl;
  } else if (strcasecmp(key, "AddressFamily") == 0) {
    char *af = NULL;
    int status = config_set_string(key, &af, value);
    if (status != 0)
      return status;

    if (strncmp(af, "any", 3) == 0) {
      ping_af = AF_UNSPEC;
    } else if (strncmp(af, "ipv4", 4) == 0) {
      ping_af = AF_INET;
    } else if (strncmp(af, "ipv6", 4) == 0) {
      ping_af = AF_INET6;
    } else {
      WARNING("ping plugin: Ignoring invalid AddressFamily value %s", af);
    }
    free(af);

  } else if (strcasecmp(key, "SourceAddress") == 0) {
    int status = config_set_string(key, &ping_source, value);
    if (status != 0)
      return status;
  }
#ifdef HAVE_OPING_1_3
  else if (strcasecmp(key, "Device") == 0) {
    int status = config_set_string(key, &ping_device, value);
    if (status != 0)
      return status;
  }
#endif
  else if (strcasecmp(key, "TTL") == 0) {
    int ttl = atoi(value);
    if ((ttl > 0) && (ttl <= 255))
      ping_ttl = ttl;
    else
      WARNING("ping plugin: Ignoring invalid TTL %i.", ttl);
  } else if (strcasecmp(key, "Interval") == 0) {
    double tmp;

    tmp = atof(value);
    if (tmp > 0.0)
      ping_interval = tmp;
    else
      WARNING("ping plugin: Ignoring invalid interval %g (%s)", tmp, value);
  } else if (strcasecmp(key, "Size") == 0) {
    size_t size = (size_t)atoi(value);

    /* Max IP packet size - (IPv6 + ICMP) = 65535 - (40 + 8) = 65487 */
    if (size <= 65487) {
      sfree(ping_data);
      ping_data = malloc(size + 1);
      if (ping_data == NULL) {
        ERROR("ping plugin: malloc failed.");
        return 1;
      }

      /* Note: By default oping is using constant string
       * "liboping -- ICMP ping library <http://octo.it/liboping/>"
       * which is exactly 56 bytes.
       *
       * Optimally we would follow the ping(1) behaviour, but we
       * cannot use byte 00 or start data payload at exactly same
       * location, due to oping library limitations. */
      for (size_t i = 0; i < size; i++) /* {{{ */
      {
        /* This restricts data pattern to be only composed of easily
         * printable characters, and not NUL character. */
        ping_data[i] = ('0' + i % 64);
      } /* }}} for (i = 0; i < size; i++) */
      ping_data[size] = 0;
    } else
      WARNING("ping plugin: Ignoring invalid Size %" PRIsz ".", size);
  } else if (strcasecmp(key, "Timeout") == 0) {
    double tmp;

    tmp = atof(value);
    if (tmp > 0.0)
      ping_timeout = tmp;
    else
      WARNING("ping plugin: Ignoring invalid timeout %g (%s)", tmp, value);
  } else if (strcasecmp(key, "MaxMissed") == 0) {
    ping_max_missed = atoi(value);
    if (ping_max_missed < 0)
      INFO("ping plugin: MaxMissed < 0, disabled re-resolving of hosts");
  } else {
    return -1;
  }

  return 0;
}

static int ping_read(void)
{
  metric_family_t fam_ping_droprate = {
      .name = "ping_droprate",
      .type = METRIC_TYPE_GAUGE,
  };
  metric_family_t fam_ping_distribution_latency = {
      .name = "ping_distribution_latency",
      .type = METRIC_TYPE_DISTRIBUTION,
  };

  metric_family_t *fams[] = {&fam_ping_distribution_latency, &fam_ping_droprate, NULL};

  if (ping_thread_error != 0) {
    ERROR("ping plugin: The ping thread had a problem. Restarting it.");

    stop_thread();

    for (hostlist_t *hl = hostlist_head; hl != NULL; hl = hl->next) {
      hl->pkg_sent = 0;
      hl->pkg_recv = 0;
      distribution_reset(hl->dist_latency);
    }

    start_thread();

    return -1;
  } /* if (ping_thread_error != 0) */

  for (hostlist_t *hl = hostlist_head; hl != NULL; hl = hl->next) /* {{{ */
  {
    uint32_t pkg_sent;
    uint32_t pkg_recv;
    double droprate;

    /* Locking here works, because the structure of the linked list is only
     * changed during configure and shutdown. */
    pthread_mutex_lock(&ping_lock);

    pkg_sent = hl->pkg_sent;
    pkg_recv = hl->pkg_recv;

    hl->pkg_sent = 0;
    hl->pkg_recv = 0;

    pthread_mutex_unlock(&ping_lock);

    /* This e. g. happens when starting up. */
    if (pkg_sent == 0) {
      DEBUG("ping plugin: No packages for host %s have been sent.", hl->host);
      continue;
    }

    /* Calculate drop rate. */
    droprate = ((double)(pkg_sent - pkg_recv)) / ((double)pkg_sent);

    metric_t m = {0};
    metric_label_set(&m, "source", hostname_g);
    metric_label_set(&m, "destination", hl->host);

    distribution_t *dist_latency = distribution_clone(hl->dist_latency);
    if (dist_latency == NULL) {
      ERROR("ping plugin: distribution_clone failed for host %s.", hl->host);
    } else {
      m.value.distribution = dist_latency;
      metric_family_metric_append(&fam_ping_distribution_latency, m);
    }

    m.value.gauge = droprate;
    metric_family_metric_append(&fam_ping_droprate, m);

    metric_reset(&m);
  } /* }}} for (hl = hostlist_head; hl != NULL; hl = hl->next) */

  for (size_t i = 0; fams[i] != NULL; i++) {
    if (fams[i]->metric.num > 0) {
      int status = plugin_dispatch_metric_family(fams[i]);
      if (status != 0) {
        ERROR("ping plugin: plugin_dispatch_metric_family failed: %s",
              STRERROR(status));
      }
      metric_family_metric_reset(fams[i]);
    }
  }

  return 0;
}

static int ping_shutdown(void)
{
  hostlist_t *hl;

  INFO("ping plugin: Shutting down thread.");
  if (stop_thread() < 0)
    return -1;

  hl = hostlist_head;
  while (hl != NULL) {
    hostlist_t *hl_next;

    hl_next = hl->next;

    sfree(hl->host);
    distribution_destroy(hl->dist_latency);
    sfree(hl);

    hl = hl_next;
  }

  if (ping_data != NULL) {
    free(ping_data);
    ping_data = NULL;
  }

  return 0;
}

void module_register(void)
{
  plugin_register_config("ping", ping_config, config_keys, config_keys_num);
  plugin_register_init("ping", ping_init);
  plugin_register_read("ping", ping_read);
  plugin_register_shutdown("ping", ping_shutdown);
}
