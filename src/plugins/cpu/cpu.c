/**
 * collectd - src/cpu.c
 * Copyright (C) 2005-2014  Florian octo Forster
 * Copyright (C) 2008       Oleg King
 * Copyright (C) 2009       Simon Kuhnle
 * Copyright (C) 2009       Manuel Sanmartin
 * Copyright (C) 2013-2014  Pierre-Yves Ritschard
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
 *   Oleg King <king2 at kaluga.ru>
 *   Simon Kuhnle <simon at blarzwurst.de>
 *   Manuel Sanmartin
 *   Pierre-Yves Ritschard <pyr at spootnik.org>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#ifdef HAVE_MACH_KERN_RETURN_H
#include <mach/kern_return.h>
#endif
#ifdef HAVE_MACH_MACH_INIT_H
#include <mach/mach_init.h>
#endif
#ifdef HAVE_MACH_HOST_PRIV_H
#include <mach/host_priv.h>
#endif
#if HAVE_MACH_MACH_ERROR_H
#include <mach/mach_error.h>
#endif
#ifdef HAVE_MACH_PROCESSOR_INFO_H
#include <mach/processor_info.h>
#endif
#ifdef HAVE_MACH_PROCESSOR_H
#include <mach/processor.h>
#endif
#ifdef HAVE_MACH_VM_MAP_H
#include <mach/vm_map.h>
#endif

#ifdef HAVE_LIBKSTAT
#include <sys/sysinfo.h>
#endif /* HAVE_LIBKSTAT */

#if (defined(HAVE_SYSCTL) && defined(HAVE_SYSCTLBYNAME)) || defined(__OpenBSD__)
/* Implies BSD variant */
#include <sys/sysctl.h>
#endif

#ifdef HAVE_SYS_DKSTAT_H
/* implies BSD variant */
#include <sys/dkstat.h>

#if !defined(CP_USER) || !defined(CP_NICE) || !defined(CP_SYS) ||              \
    !defined(CP_INTR) || !defined(CP_IDLE) || !defined(CPUSTATES)
#define CP_USER 0
#define CP_NICE 1
#define CP_SYS 2
#define CP_INTR 3
#define CP_IDLE 4
#define CPUSTATES 5
#endif
#endif /* HAVE_SYS_DKSTAT_H */

#if (defined(HAVE_SYSCTL) && defined(HAVE_SYSCTLBYNAME)) || defined(__OpenBSD__)
/* Implies BSD variant */
#if defined(CTL_HW) && defined(HW_NCPU) && defined(CTL_KERN) &&                \
    (defined(KERN_CPTIME) || defined(KERN_CP_TIME)) && defined(CPUSTATES)
#define CAN_USE_SYSCTL 1
#else
#define CAN_USE_SYSCTL 0
#endif
#else
#define CAN_USE_SYSCTL 0
#endif /* HAVE_SYSCTL_H && HAVE_SYSCTLBYNAME || __OpenBSD__ */

#define COLLECTD_CPU_STATE_USER 0
#define COLLECTD_CPU_STATE_SYSTEM 1
#define COLLECTD_CPU_STATE_WAIT 2
#define COLLECTD_CPU_STATE_NICE 3
#define COLLECTD_CPU_STATE_SWAP 4
#define COLLECTD_CPU_STATE_INTERRUPT 5
#define COLLECTD_CPU_STATE_SOFTIRQ 6
#define COLLECTD_CPU_STATE_STEAL 7
#define COLLECTD_CPU_STATE_GUEST 8
#define COLLECTD_CPU_STATE_GUEST_NICE 9
#define COLLECTD_CPU_STATE_IDLE 10
#define COLLECTD_CPU_STATE_ACTIVE 11 /* sum of (!idle) */
#define COLLECTD_CPU_STATE_MAX 12    /* #states */

#if HAVE_STATGRAB_H
#include <statgrab.h>
#endif

#ifdef HAVE_PERFSTAT
#include <libperfstat.h>
#include <sys/protosw.h>
#endif /* HAVE_PERFSTAT */

#if !PROCESSOR_CPU_LOAD_INFO && !KERNEL_LINUX && !HAVE_LIBKSTAT &&             \
    !CAN_USE_SYSCTL && !HAVE_SYSCTLBYNAME && !HAVE_LIBSTATGRAB &&              \
    !HAVE_PERFSTAT
#error "No applicable input method."
#endif

static const char *cpu_state_names[] = {
    "user",    "system", "wait",  "nice",       "swap", "interrupt",
    "softirq", "steal",  "guest", "guest_nice", "idle", "active"};

#ifdef PROCESSOR_CPU_LOAD_INFO
static mach_port_t port_host;
static processor_port_array_t cpu_list;
static mach_msg_type_number_t cpu_list_len;
/* #endif PROCESSOR_CPU_LOAD_INFO */

#elif defined(KERNEL_LINUX)
/* no variables needed */
/* #endif KERNEL_LINUX */

#elif defined(HAVE_LIBKSTAT)
#if HAVE_KSTAT_H
#include <kstat.h>
#endif
/* colleague tells me that Sun doesn't sell systems with more than 100 or so
 * CPUs.. */
#define MAX_NUMCPU 256
extern kstat_ctl_t *kc;
static kstat_t *ksp[MAX_NUMCPU];
static int numcpu;
/* #endif HAVE_LIBKSTAT */

#elif CAN_USE_SYSCTL
/* Only possible for (Open) BSD variant */
static int numcpu;
/* #endif CAN_USE_SYSCTL */

#elif defined(HAVE_SYSCTLBYNAME)
/* Implies BSD variant */
static int numcpu;
#ifdef HAVE_SYSCTL_KERN_CP_TIMES
static int maxcpu;
#endif /* HAVE_SYSCTL_KERN_CP_TIMES */
/* #endif HAVE_SYSCTLBYNAME */

#elif defined(HAVE_LIBSTATGRAB)
/* no variables needed */
/* #endif  HAVE_LIBSTATGRAB */

#elif defined(HAVE_PERFSTAT)
#define TOTAL_IDLE 0
#define TOTAL_USER 1
#define TOTAL_SYS 2
#define TOTAL_WAIT 3
#define TOTAL_STAT_NUM 4
static value_to_rate_state_t total_conv[TOTAL_STAT_NUM];
static perfstat_cpu_t *perfcpu;
static int numcpu;
static int pnumcpu;
#endif /* HAVE_PERFSTAT */

#define RATE_ADD(sum, val)                                                     \
  do {                                                                         \
    if (isnan(sum))                                                            \
      (sum) = (val);                                                           \
    else if (!isnan(val))                                                      \
      (sum) += (val);                                                          \
  } while (0)

struct cpu_state_s {
  value_to_rate_state_t conv;
  gauge_t rate;
  bool has_value;
};
typedef struct cpu_state_s cpu_state_t;

static cpu_state_t *cpu_states;
static size_t cpu_states_num; /* #cpu_states allocated */

#if defined(KERNEL_LINUX)
typedef struct {
  char node[8];
  char socket[8];
  char core[8];
  char drawer[8];
  char book[8];
} cpu_topology_t;

static cpu_topology_t *cpu_topology;
static size_t cpu_topology_num;
#endif

/* Highest CPU number in the current iteration. Used by the dispatch logic to
 * determine how many CPUs there were. Reset to 0 by cpu_reset(). */
static size_t global_cpu_num;

static bool report_by_cpu = true;
static bool report_by_state = true;
static bool report_percent;
static bool report_num_cpu;
static bool report_guest;
static bool report_topology;
static bool subtract_guest = true;

static const char *config_keys[] = {"ReportByCpu",      "ReportByState",
                                    "ReportNumCpu",     "ValuesPercentage",
                                    "ReportGuestState", "SubtractGuestState",
                                    "ReportTopology"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

static int cpu_config(char const *key, char const *value)
{
  if (strcasecmp(key, "ReportByCpu") == 0)
    report_by_cpu = IS_TRUE(value);
  else if (strcasecmp(key, "ValuesPercentage") == 0)
    report_percent = IS_TRUE(value);
  else if (strcasecmp(key, "ReportByState") == 0)
    report_by_state = IS_TRUE(value);
  else if (strcasecmp(key, "ReportNumCpu") == 0)
    report_num_cpu = IS_TRUE(value);
  else if (strcasecmp(key, "ReportGuestState") == 0)
    report_guest = IS_TRUE(value);
  else if (strcasecmp(key, "ReportTopology") == 0)
    report_topology = IS_TRUE(value);
  else if (strcasecmp(key, "SubtractGuestState") == 0)
    subtract_guest = IS_TRUE(value);
  else
    return -1;

  return 0;
}

#if defined(KERNEL_LINUX)

#ifndef CPU_ROOT_DIR
#define CPU_ROOT_DIR "/sys/devices/system/cpu"
#endif

#ifndef NUMA_ROOT_DIR
#define NUMA_ROOT_DIR "/sys/devices/system/node"
#endif

static int cpu_topology_alloc(size_t cpu_num)
{
  cpu_topology_t *tmp;

  size_t size = (size_t)cpu_num + 1;
  assert(size > 0);

  if (cpu_topology_num >= size)
    return 0;

  tmp = realloc(cpu_topology, size * sizeof(*cpu_topology));
  if (tmp == NULL) {
    ERROR("cpu plugin: realloc failed.");
    return ENOMEM;
  }
  cpu_topology = tmp;

  memset(cpu_topology + cpu_topology_num, 0,
         (size - cpu_topology_num) * sizeof(*cpu_topology));
  cpu_topology_num = size;
  return 0;
}

static cpu_topology_t *get_cpu_topology(size_t cpu_num)
{
  if (cpu_num >= cpu_topology_num) {
    cpu_topology_alloc(cpu_num);
    if (cpu_num >= cpu_topology_num)
      return NULL;
  }
  return &cpu_topology[cpu_num];
}

static int cpu_topology_id (char const *cpu, char const *id, char *buffer, size_t size)
{
  char path[PATH_MAX];
  ssnprintf(path, sizeof(path), CPU_ROOT_DIR "/%s/topology/%s", cpu, id);

  int rsize = read_file_contents(path, buffer, size);
  if (rsize <= 0) {
    buffer[0] = '\0';
    return -1;
  }
  if (rsize > size)
    rsize = size;
  buffer[rsize - 1] = '\0';

  size_t len = strlen(buffer);
  if ((len > 0) && (buffer[len -1] == '\n'))
    buffer[len -1] = '\0';
  return 0;
}

static int cpu_topology_id_callback (char const *dir, char const *cpu, void *user_data)
{
  if (strncmp(cpu, "cpu", 3) != 0)
    return 0;
  if (!isdigit(cpu[3]))
    return 0;

  int cpu_num = atoi(cpu + 3);

  int status = cpu_topology_alloc(cpu_num);
  if (status != 0)
    return status;

  cpu_topology_t *t = get_cpu_topology(cpu_num);
  if (t == NULL)
    return 0;

  cpu_topology_id(cpu, "core_id", t->core, sizeof(t->core));
  cpu_topology_id(cpu, "physical_package_id", t->socket, sizeof(t->socket));
  cpu_topology_id(cpu, "book_id", t->book, sizeof(t->book));
  cpu_topology_id(cpu, "drawer_id", t->drawer, sizeof(t->drawer));
  return 0;
}

static void cpu_topology_set_node(int ncpu, char const *node, int set)
{
  cpu_topology_t *t = get_cpu_topology(ncpu);
  if (t == NULL)
    return;
  if (set)
    sstrncpy(t->node, node, sizeof(t->node));
  else
    t->node[0] = '\0';
}

static inline int hex_to_int(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f')
    return (c - 'a') + 10;
  return -1;
}

static int cpu_topology_node_callback (char const *dir, char const *node,
                                       void *user_data)
{
  if (strncmp(node, "node", 4) != 0)
    return 0;
  if (!isdigit(node[4]))
    return 0;

  char path[PATH_MAX];
  ssnprintf(path, sizeof(path), NUMA_ROOT_DIR "/%s/cpumap", node);

  char buffer[8096];
  ssize_t rsize = read_file_contents(path, buffer, sizeof(buffer));
  if (rsize <= 0) {
    buffer[0] = '\0';
    return -1;
  }
  if (rsize > sizeof(buffer))
    rsize = sizeof(buffer);
  buffer[rsize - 1] = '\0';

  size_t len = strlen(buffer);
  if ((len > 0) && (buffer[len -1] == '\n'))
    buffer[len -1] = '\0';

  int ncpu = 0;
  for (char *c = buffer + strlen(buffer) - 1;  c >= buffer; c--) {
    if (*c == 'x')
      break;
    if (*c == ',')
      c--;

    int set = hex_to_int(*c);
    if (set < 0)
      continue;

    cpu_topology_set_node(ncpu, node + 4, set & 1);
    cpu_topology_set_node(ncpu+1, node + 4, set & 2);
    cpu_topology_set_node(ncpu+2, node + 4, set & 4);
    cpu_topology_set_node(ncpu+3, node + 4, set & 8);
    ncpu += 4;
  }

  return 0;
}

static int cpu_topology_set_labels(int ncpu, metric_t *m)
{
  cpu_topology_t *t = get_cpu_topology(ncpu);
  if (t == NULL)
    return 0;

  if (t->core[0] != '\0')
    metric_label_set(m, "core", t->core);
  if (t->socket[0] != '\0')
    metric_label_set(m, "socket", t->socket);
  if (t->book[0] != '\0')
    metric_label_set(m, "book", t->book);
  if (t->drawer[0] != '\0')
    metric_label_set(m, "drawer", t->drawer);
  if (t->node[0] != '\0')
    metric_label_set(m, "node", t->node);

  return 0;
}

static int cpu_topology_scan(void)
{
  if (cpu_topology_num > 0)
    memset(cpu_topology, 0, cpu_topology_num * sizeof(*cpu_topology));

  walk_directory(CPU_ROOT_DIR, cpu_topology_id_callback, NULL, 0);

  walk_directory(NUMA_ROOT_DIR, cpu_topology_node_callback, NULL, 0);

  return 0;
}

#endif /* defined(KERNEL_LINUX) */

static int init(void) {
#if PROCESSOR_CPU_LOAD_INFO
  kern_return_t status;

  port_host = mach_host_self();

  status = host_processors(port_host, &cpu_list, &cpu_list_len);
  if (status == KERN_INVALID_ARGUMENT) {
    ERROR("cpu plugin: Don't have a privileged host control port. "
          "The most common cause for this problem is "
          "that collectd is running without root "
          "privileges, which are required to read CPU "
          "load information. "
          "<https://collectd.org/bugs/22>");
    cpu_list_len = 0;
    return -1;
  }
  if (status != KERN_SUCCESS) {
    ERROR("cpu plugin: host_processors() failed with status %d.", (int)status);
    cpu_list_len = 0;
    return -1;
  }

  INFO("cpu plugin: Found %i processor%s.", (int)cpu_list_len,
       cpu_list_len == 1 ? "" : "s");
  /* #endif PROCESSOR_CPU_LOAD_INFO */

#elif defined(HAVE_LIBKSTAT)
  kstat_t *ksp_chain;

  numcpu = 0;

  if (kc == NULL)
    return -1;

  /* Solaris doesn't count linear.. *sigh* */
  for (numcpu = 0, ksp_chain = kc->kc_chain;
       (numcpu < MAX_NUMCPU) && (ksp_chain != NULL);
       ksp_chain = ksp_chain->ks_next)
    if (strncmp(ksp_chain->ks_module, "cpu_stat", 8) == 0)
      ksp[numcpu++] = ksp_chain;
      /* #endif HAVE_LIBKSTAT */

#elif CAN_USE_SYSCTL
  /* Only on (Open) BSD variant */
  size_t numcpu_size;
  int mib[2] = {CTL_HW, HW_NCPU};
  int status;

  numcpu = 0;
  numcpu_size = sizeof(numcpu);

  status = sysctl(mib, STATIC_ARRAY_SIZE(mib), &numcpu, &numcpu_size, NULL, 0);
  if (status == -1) {
    WARNING("cpu plugin: sysctl: %s", STRERRNO);
    return -1;
  }
  /* #endif CAN_USE_SYSCTL */

#elif defined(HAVE_SYSCTLBYNAME)
  /* Only on BSD varient */
  size_t numcpu_size;

  numcpu_size = sizeof(numcpu);

  if (sysctlbyname("hw.ncpu", &numcpu, &numcpu_size, NULL, 0) < 0) {
    WARNING("cpu plugin: sysctlbyname(hw.ncpu): %s", STRERRNO);
    return -1;
  }

#ifdef HAVE_SYSCTL_KERN_CP_TIMES
  numcpu_size = sizeof(maxcpu);

  if (sysctlbyname("kern.smp.maxcpus", &maxcpu, &numcpu_size, NULL, 0) < 0) {
    WARNING("cpu plugin: sysctlbyname(kern.smp.maxcpus): %s", STRERRNO);
    return -1;
  }
#else
  if (numcpu != 1)
    NOTICE("cpu: Only one processor supported when using `sysctlbyname' (found "
           "%i)",
           numcpu);
#endif
  /* #endif HAVE_SYSCTLBYNAME */

#elif defined(HAVE_LIBSTATGRAB)
  /* nothing to initialize */
  /* #endif HAVE_LIBSTATGRAB */

#elif defined(HAVE_PERFSTAT)
/* nothing to initialize */
/* #endif HAVE_PERFSTAT */

#elif defined(KERNEL_LINUX)
  cpu_topology_scan();
#endif /* KERNEL_LINUX */

  return 0;
} /* int init */

/* Takes the zero-index number of a CPU and makes sure that the module-global
 * cpu_states buffer is large enough. Returne ENOMEM on erorr. */
static int cpu_states_alloc(size_t cpu_num) /* {{{ */
{
  cpu_state_t *tmp;
  size_t sz;

  sz = (((size_t)cpu_num) + 1) * COLLECTD_CPU_STATE_MAX;
  assert(sz > 0);

  /* We already have enough space. */
  if (cpu_states_num >= sz)
    return 0;

  tmp = realloc(cpu_states, sz * sizeof(*cpu_states));
  if (tmp == NULL) {
    ERROR("cpu plugin: realloc failed.");
    return ENOMEM;
  }
  cpu_states = tmp;
  tmp = cpu_states + cpu_states_num;

  memset(tmp, 0, (sz - cpu_states_num) * sizeof(*cpu_states));
  cpu_states_num = sz;
  return 0;
}

static cpu_state_t *get_cpu_state(size_t cpu_num, size_t state)
{
  size_t index = ((cpu_num * COLLECTD_CPU_STATE_MAX) + state);

  if (index >= cpu_states_num)
    return NULL;

  return &cpu_states[index];
} /* }}} cpu_state_t *get_cpu_state */

#if defined(HAVE_PERFSTAT) /* {{{ */
/* populate global aggregate cpu rate */
static int total_rate(gauge_t *sum_by_state, size_t state, derive_t d,
                      value_to_rate_state_t *conv, cdtime_t now)
{
  gauge_t rate = NAN;
  int status =
      value_to_rate(&rate, (value_t){.derive = d}, DS_TYPE_DERIVE, now, conv);
  if (status != 0)
    return status;

  sum_by_state[state] = rate;

  if (state != COLLECTD_CPU_STATE_IDLE)
    RATE_ADD(sum_by_state[COLLECTD_CPU_STATE_ACTIVE], sum_by_state[state]);
  return 0;
}
#endif /* }}} HAVE_PERFSTAT */

/* Populates the per-CPU COLLECTD_CPU_STATE_ACTIVE rate and the global
 * rate_by_state
 * array. */
static void aggregate(gauge_t *sum_by_state)
{
  for (size_t state = 0; state < COLLECTD_CPU_STATE_MAX; state++)
    sum_by_state[state] = NAN;

  for (size_t cpu_num = 0; cpu_num < global_cpu_num; cpu_num++) {
    cpu_state_t *this_cpu_states = get_cpu_state(cpu_num, 0);

    this_cpu_states[COLLECTD_CPU_STATE_ACTIVE].rate = NAN;

    for (size_t state = 0; state < COLLECTD_CPU_STATE_ACTIVE; state++) {
      if (!this_cpu_states[state].has_value)
        continue;

      RATE_ADD(sum_by_state[state], this_cpu_states[state].rate);
      if (state != COLLECTD_CPU_STATE_IDLE)
        RATE_ADD(this_cpu_states[COLLECTD_CPU_STATE_ACTIVE].rate,
                 this_cpu_states[state].rate);
    }

    if (!isnan(this_cpu_states[COLLECTD_CPU_STATE_ACTIVE].rate))
      this_cpu_states[COLLECTD_CPU_STATE_ACTIVE].has_value = true;

    RATE_ADD(sum_by_state[COLLECTD_CPU_STATE_ACTIVE],
             this_cpu_states[COLLECTD_CPU_STATE_ACTIVE].rate);
  }

#if defined(HAVE_PERFSTAT) /* {{{ */
  cdtime_t now = cdtime();
  perfstat_cpu_total_t cputotal = {0};

  if (!perfstat_cpu_total(NULL, &cputotal, sizeof(cputotal), 1)) {
    WARNING("cpu plugin: perfstat_cpu_total: %s", STRERRNO);
    return;
  }

  /* Reset COLLECTD_CPU_STATE_ACTIVE */
  sum_by_state[COLLECTD_CPU_STATE_ACTIVE] = NAN;

  /* Physical Processor Utilization */
  total_rate(sum_by_state, COLLECTD_CPU_STATE_IDLE, (derive_t)cputotal.pidle,
             &total_conv[TOTAL_IDLE], now);
  total_rate(sum_by_state, COLLECTD_CPU_STATE_USER, (derive_t)cputotal.puser,
             &total_conv[TOTAL_USER], now);
  total_rate(sum_by_state, COLLECTD_CPU_STATE_SYSTEM, (derive_t)cputotal.psys,
             &total_conv[TOTAL_SYS], now);
  total_rate(sum_by_state, COLLECTD_CPU_STATE_WAIT, (derive_t)cputotal.pwait,
             &total_conv[TOTAL_WAIT], now);
#endif /* }}} HAVE_PERFSTAT */
}

static void cpu_commit_metric(metric_family_t *fam, metric_t *m,
                              gauge_t rates[static COLLECTD_CPU_STATE_MAX])
{
  gauge_t sum = rates[COLLECTD_CPU_STATE_ACTIVE];
  RATE_ADD(sum, rates[COLLECTD_CPU_STATE_IDLE]);

  if (!report_by_state) {
    m->value.gauge = 100.0 * rates[COLLECTD_CPU_STATE_ACTIVE] / sum;
    metric_label_set(m, "state", cpu_state_names[COLLECTD_CPU_STATE_ACTIVE]);
    metric_family_metric_append(fam, *m);
  } else {
    for (size_t state = 0; state < COLLECTD_CPU_STATE_ACTIVE; state++) {
      m->value.gauge = 100.0 * rates[state] / sum;
      metric_label_set(m, "state", cpu_state_names[state]);
      metric_family_metric_append(fam, *m);
    }
  }

  int status = plugin_dispatch_metric_family(fam);
  if (status != 0) {
    ERROR("cpu plugin: plugin_dispatch_metric_family failed: %s",
          STRERROR(status));
  }

  metric_reset(m);
  metric_family_metric_reset(fam);
}

static void cpu_commit_all(gauge_t rates[static COLLECTD_CPU_STATE_MAX])
{
  metric_family_t fam = {
      .name = "host_cpu_all_usage_percent",
      .type = METRIC_TYPE_GAUGE,
  };

  metric_t m = {0};
  cpu_commit_metric(&fam, &m, rates);
}

/* Commits (dispatches) the values for one CPU or the global aggregation.
 * cpu_num is the index of the CPU to be committed or -1 in case of the global
 * aggregation. rates is a pointer to COLLECTD_CPU_STATE_MAX gauge_t values
 * holding the
 * current rate; each rate may be NAN. Calculates the percentage of each state
 * and dispatches the metric. */
static void cpu_commit_one(int cpu_num, /* {{{ */
                           gauge_t rates[static COLLECTD_CPU_STATE_MAX])
{
  metric_family_t fam = {
      .name = "host_cpu_usage_percent",
      .type = METRIC_TYPE_GAUGE,
  };

  metric_t m = {0};
  char cpu_num_str[16];
  snprintf(cpu_num_str, sizeof(cpu_num_str), "%d", cpu_num);
  metric_label_set(&m, "cpu", cpu_num_str);
#if defined(KERNEL_LINUX)
  if (report_topology) {
    if ((cpu_topology_num * COLLECTD_CPU_STATE_MAX) != cpu_states_num)
      cpu_topology_scan();
    cpu_topology_set_labels(cpu_num, &m);
  }
#endif

  cpu_commit_metric(&fam, &m, rates);
}

/* Commits the number of cores */
static void cpu_commit_num_cpu(gauge_t value) /* {{{ */
{
  metric_family_t fam = {
      .name = "host_cpu_count",
      .type = METRIC_TYPE_GAUGE,
  };
  metric_family_metric_append(&fam, (metric_t){
                                        .value.gauge = value,
                                    });

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0) {
    ERROR("plugin_dispatch_metric_family failed: %s", STRERROR(status));
  }

  metric_family_metric_reset(&fam);
  return;
} /* }}} void cpu_commit_num_cpu */

/* Resets the internal aggregation. This is called by the read callback after
 * each iteration / after each call to cpu_commit(). */
static void cpu_reset(void) /* {{{ */
{
  for (size_t i = 0; i < cpu_states_num; i++)
    cpu_states[i].has_value = false;

  global_cpu_num = 0;
} /* }}} void cpu_reset */


static void cpu_commit_all_without_aggregation(void)
{
  metric_family_t fam = {
      .name = "host_cpu_all_usage_total",
      .type = METRIC_TYPE_COUNTER,
  };

  metric_t m = {0};

  derive_t values[COLLECTD_CPU_STATE_ACTIVE] = {0};
  for (int state = 0; state < COLLECTD_CPU_STATE_ACTIVE; state++) {
    for (size_t cpu_num = 0; cpu_num < global_cpu_num; cpu_num++) {
      cpu_state_t *s = get_cpu_state(cpu_num, state);
      if (s == NULL)
        continue;
      if (!s->has_value)
        continue;
      values[state] += s->conv.last_value.derive;
    }
  }

  for (int state = 0; state < COLLECTD_CPU_STATE_ACTIVE; state++) {
    metric_label_set(&m, "state", cpu_state_names[state]);
    m.value.derive = values[state];
    metric_family_metric_append(&fam, m);
  }

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0) {
    ERROR("plugin_dispatch_metric_family failed: %s", STRERROR(status));
  }

  metric_reset(&m);
  metric_family_metric_reset(&fam);
}

static void cpu_commit_without_aggregation(void)
{
  metric_family_t fam = {
      .name = "host_cpu_usage_total",
      .type = METRIC_TYPE_COUNTER,
  };

  metric_t m = {0};
#if defined(KERNEL_LINUX)
  if (report_topology) {
    if ((cpu_topology_num * COLLECTD_CPU_STATE_MAX) != cpu_states_num)
      cpu_topology_scan();
  }
#endif

  for (int state = 0; state < COLLECTD_CPU_STATE_ACTIVE; state++) {
    metric_label_set(&m, "state", cpu_state_names[state]);

    for (size_t cpu_num = 0; cpu_num < global_cpu_num; cpu_num++) {
      cpu_state_t *s = get_cpu_state(cpu_num, state);

      if (!s->has_value)
        continue;

      char cpu_num_str[16];
      snprintf(cpu_num_str, sizeof(cpu_num_str), "%zu", cpu_num);
      metric_label_set(&m, "cpu", cpu_num_str);

#if defined(KERNEL_LINUX)
      if (report_topology)
        cpu_topology_set_labels(cpu_num, &m);
#endif
      m.value.derive = s->conv.last_value.derive;

      metric_family_metric_append(&fam, m);
    }
  }

  if (fam.metric.num == 0)
    return;

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0) {
    ERROR("plugin_dispatch_metric_family failed: %s", STRERROR(status));
  }

  metric_reset(&m);
  metric_family_metric_reset(&fam);
}

/* Aggregates the internal state and dispatches the metrics. */
static void cpu_commit(void) /* {{{ */
{
  gauge_t global_rates[COLLECTD_CPU_STATE_MAX] = {
      NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN /* Batman! */
  };

  if (report_num_cpu)
    cpu_commit_num_cpu((gauge_t)global_cpu_num);

  if (!report_percent) {
    cpu_commit_all_without_aggregation();
    if (report_by_cpu)
      cpu_commit_without_aggregation();
    return;
  }

  aggregate(global_rates);

  cpu_commit_all(global_rates);
  if (!report_by_cpu)
    return;

  for (size_t cpu_num = 0; cpu_num < global_cpu_num; cpu_num++) {
    cpu_state_t *this_cpu_states = get_cpu_state(cpu_num, 0);
    gauge_t local_rates[COLLECTD_CPU_STATE_MAX] = {
        NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};

    for (size_t state = 0; state < COLLECTD_CPU_STATE_MAX; state++)
      if (this_cpu_states[state].has_value)
        local_rates[state] = this_cpu_states[state].rate;

    cpu_commit_one((int)cpu_num, local_rates);
  }
} /* }}} void cpu_commit */

/* Adds a derive value to the internal state. This should be used by each read
 * function for each state. At the end of the iteration, the read function
 * should call cpu_commit(). */
static int cpu_stage(size_t cpu_num, size_t state, derive_t d,
                     cdtime_t now) /* {{{ */
{
  int status;
  cpu_state_t *s;
  gauge_t rate = NAN;
  value_t val = {.derive = d};

  if (state >= COLLECTD_CPU_STATE_ACTIVE)
    return EINVAL;

  status = cpu_states_alloc(cpu_num);
  if (status != 0)
    return status;

  if (global_cpu_num <= cpu_num)
    global_cpu_num = cpu_num + 1;

  s = get_cpu_state(cpu_num, state);

  status = value_to_rate(&rate, val, DS_TYPE_DERIVE, now, &s->conv);
  if (status != 0)
    return status;

  s->rate = rate;
  s->has_value = true;
  return 0;
} /* }}} int cpu_stage */

static int cpu_read(void) {
  cdtime_t now = cdtime();

#if PROCESSOR_CPU_LOAD_INFO /* {{{ */
  kern_return_t status;

  processor_cpu_load_info_data_t cpu_info;
  mach_msg_type_number_t cpu_info_len;

  host_t cpu_host;

  for (mach_msg_type_number_t cpu = 0; cpu < cpu_list_len; cpu++) {
    cpu_host = 0;
    cpu_info_len = PROCESSOR_BASIC_INFO_COUNT;

    status = processor_info(cpu_list[cpu], PROCESSOR_CPU_LOAD_INFO, &cpu_host,
                            (processor_info_t)&cpu_info, &cpu_info_len);
    if (status != KERN_SUCCESS) {
      ERROR("cpu plugin: processor_info (PROCESSOR_CPU_LOAD_INFO) failed: %s",
            mach_error_string(status));
      continue;
    }

    if (cpu_info_len < CPU_STATE_MAX) {
      ERROR("cpu plugin: processor_info returned only %i elements..",
            cpu_info_len);
      continue;
    }

    cpu_stage(cpu, COLLECTD_CPU_STATE_USER,
              (derive_t)cpu_info.cpu_ticks[CPU_STATE_USER], now);
    cpu_stage(cpu, COLLECTD_CPU_STATE_NICE,
              (derive_t)cpu_info.cpu_ticks[CPU_STATE_NICE], now);
    cpu_stage(cpu, COLLECTD_CPU_STATE_SYSTEM,
              (derive_t)cpu_info.cpu_ticks[CPU_STATE_SYSTEM], now);
    cpu_stage(cpu, COLLECTD_CPU_STATE_IDLE,
              (derive_t)cpu_info.cpu_ticks[CPU_STATE_IDLE], now);
  }
  /* }}} #endif PROCESSOR_CPU_LOAD_INFO */

#elif defined(KERNEL_LINUX) /* {{{ */
  int cpu;
  FILE *fh;
  char buf[1024];

  char *fields[11];
  int numfields;

  if ((fh = fopen("/proc/stat", "r")) == NULL) {
    ERROR("cpu plugin: fopen (/proc/stat) failed: %s", STRERRNO);
    return -1;
  }

  while (fgets(buf, 1024, fh) != NULL) {
    if (strncmp(buf, "cpu", 3))
      continue;
    if ((buf[3] < '0') || (buf[3] > '9'))
      continue;

    numfields = strsplit(buf, fields, STATIC_ARRAY_SIZE(fields));
    if (numfields < 5)
      continue;

    cpu = atoi(fields[0] + 3);

    /* Do not stage User and Nice immediately: we may need to alter them later:
     */
    long long user_value = atoll(fields[1]);
    long long nice_value = atoll(fields[2]);
    cpu_stage(cpu, COLLECTD_CPU_STATE_SYSTEM, (derive_t)atoll(fields[3]), now);
    cpu_stage(cpu, COLLECTD_CPU_STATE_IDLE, (derive_t)atoll(fields[4]), now);

    if (numfields >= 8) {
      cpu_stage(cpu, COLLECTD_CPU_STATE_WAIT, (derive_t)atoll(fields[5]), now);
      cpu_stage(cpu, COLLECTD_CPU_STATE_INTERRUPT, (derive_t)atoll(fields[6]),
                now);
      cpu_stage(cpu, COLLECTD_CPU_STATE_SOFTIRQ, (derive_t)atoll(fields[7]),
                now);
    }

    if (numfields >= 9) { /* Steal (since Linux 2.6.11) */
      cpu_stage(cpu, COLLECTD_CPU_STATE_STEAL, (derive_t)atoll(fields[8]), now);
    }

    if (numfields >= 10) { /* Guest (since Linux 2.6.24) */
      if (report_guest) {
        long long value = atoll(fields[9]);
        cpu_stage(cpu, COLLECTD_CPU_STATE_GUEST, (derive_t)value, now);
        /* Guest is included in User; optionally subtract Guest from User: */
        if (subtract_guest) {
          user_value -= value;
          if (user_value < 0)
            user_value = 0;
        }
      }
    }

    if (numfields >= 11) { /* Guest_nice (since Linux 2.6.33) */
      if (report_guest) {
        long long value = atoll(fields[10]);
        cpu_stage(cpu, COLLECTD_CPU_STATE_GUEST_NICE, (derive_t)value, now);
        /* Guest_nice is included in Nice; optionally subtract Guest_nice from
           Nice: */
        if (subtract_guest) {
          nice_value -= value;
          if (nice_value < 0)
            nice_value = 0;
        }
      }
    }

    /* Eventually stage User and Nice: */
    cpu_stage(cpu, COLLECTD_CPU_STATE_USER, (derive_t)user_value, now);
    cpu_stage(cpu, COLLECTD_CPU_STATE_NICE, (derive_t)nice_value, now);
  }
  fclose(fh);
  /* }}} #endif defined(KERNEL_LINUX) */

#elif defined(HAVE_LIBKSTAT) /* {{{ */
  static cpu_stat_t cs;

  if (kc == NULL)
    return -1;

  for (int cpu = 0; cpu < numcpu; cpu++) {
    if (kstat_read(kc, ksp[cpu], &cs) == -1)
      continue; /* error message? */

    cpu_stage(ksp[cpu]->ks_instance, COLLECTD_CPU_STATE_IDLE,
              (derive_t)cs.cpu_sysinfo.cpu[CPU_IDLE], now);
    cpu_stage(ksp[cpu]->ks_instance, COLLECTD_CPU_STATE_USER,
              (derive_t)cs.cpu_sysinfo.cpu[CPU_USER], now);
    cpu_stage(ksp[cpu]->ks_instance, COLLECTD_CPU_STATE_SYSTEM,
              (derive_t)cs.cpu_sysinfo.cpu[CPU_KERNEL], now);
    cpu_stage(ksp[cpu]->ks_instance, COLLECTD_CPU_STATE_WAIT,
              (derive_t)cs.cpu_sysinfo.cpu[CPU_WAIT], now);
  }
  /* }}} #endif defined(HAVE_LIBKSTAT) */

#elif CAN_USE_SYSCTL /* {{{ */
  /* Only on (Open) BSD variant */
  uint64_t cpuinfo[numcpu][CPUSTATES];
  size_t cpuinfo_size;
  int status;

  if (numcpu < 1) {
    ERROR("cpu plugin: Could not determine number of "
          "installed CPUs using sysctl(3).");
    return -1;
  }

  memset(cpuinfo, 0, sizeof(cpuinfo));

#if defined(KERN_CP_TIME) && defined(KERNEL_NETBSD)
  {
    int mib[] = {CTL_KERN, KERN_CP_TIME};

    cpuinfo_size = sizeof(cpuinfo[0]) * numcpu * CPUSTATES;
    status = sysctl(mib, 2, cpuinfo, &cpuinfo_size, NULL, 0);
    if (status == -1) {
      char errbuf[1024];

      ERROR("cpu plugin: sysctl failed: %s.",
            sstrerror(errno, errbuf, sizeof(errbuf)));
      return -1;
    }
    if (cpuinfo_size == (sizeof(cpuinfo[0]) * CPUSTATES)) {
      numcpu = 1;
    }
  }
#else /* defined(KERN_CP_TIME) && defined(KERNEL_NETBSD) */
#if defined(KERN_CPTIME2)
  if (numcpu > 1) {
    for (int i = 0; i < numcpu; i++) {
      int mib[] = {CTL_KERN, KERN_CPTIME2, i};

      cpuinfo_size = sizeof(cpuinfo[0]);

      status = sysctl(mib, STATIC_ARRAY_SIZE(mib), cpuinfo[i], &cpuinfo_size,
                      NULL, 0);
      if (status == -1) {
        ERROR("cpu plugin: sysctl failed: %s.", STRERRNO);
        return -1;
      }
    }
  } else
#endif /* defined(KERN_CPTIME2) */
  {
    int mib[] = {CTL_KERN, KERN_CPTIME};
    long cpuinfo_tmp[CPUSTATES];

    cpuinfo_size = sizeof(cpuinfo_tmp);

    status = sysctl(mib, STATIC_ARRAY_SIZE(mib), &cpuinfo_tmp, &cpuinfo_size,
                    NULL, 0);
    if (status == -1) {
      ERROR("cpu plugin: sysctl failed: %s.", STRERRNO);
      return -1;
    }

    for (int i = 0; i < CPUSTATES; i++) {
      cpuinfo[0][i] = cpuinfo_tmp[i];
    }
  }
#endif /* defined(KERN_CP_TIME) && defined(KERNEL_NETBSD) */

  for (int i = 0; i < numcpu; i++) {
    cpu_stage(i, COLLECTD_CPU_STATE_USER, (derive_t)cpuinfo[i][CP_USER], now);
    cpu_stage(i, COLLECTD_CPU_STATE_NICE, (derive_t)cpuinfo[i][CP_NICE], now);
    cpu_stage(i, COLLECTD_CPU_STATE_SYSTEM, (derive_t)cpuinfo[i][CP_SYS], now);
    cpu_stage(i, COLLECTD_CPU_STATE_IDLE, (derive_t)cpuinfo[i][CP_IDLE], now);
    cpu_stage(i, COLLECTD_CPU_STATE_INTERRUPT, (derive_t)cpuinfo[i][CP_INTR],
              now);
  }
  /* }}} #endif CAN_USE_SYSCTL */

#elif defined(HAVE_SYSCTLBYNAME) && defined(HAVE_SYSCTL_KERN_CP_TIMES) /* {{{  \
                                                                        */
  /* Only on BSD variant */
  long cpuinfo[maxcpu][CPUSTATES];
  size_t cpuinfo_size;

  memset(cpuinfo, 0, sizeof(cpuinfo));

  cpuinfo_size = sizeof(cpuinfo);
  if (sysctlbyname("kern.cp_times", &cpuinfo, &cpuinfo_size, NULL, 0) < 0) {
    ERROR("cpu plugin: sysctlbyname failed: %s.", STRERRNO);
    return -1;
  }

  for (int i = 0; i < numcpu; i++) {
    cpu_stage(i, COLLECTD_CPU_STATE_USER, (derive_t)cpuinfo[i][CP_USER], now);
    cpu_stage(i, COLLECTD_CPU_STATE_NICE, (derive_t)cpuinfo[i][CP_NICE], now);
    cpu_stage(i, COLLECTD_CPU_STATE_SYSTEM, (derive_t)cpuinfo[i][CP_SYS], now);
    cpu_stage(i, COLLECTD_CPU_STATE_IDLE, (derive_t)cpuinfo[i][CP_IDLE], now);
    cpu_stage(i, COLLECTD_CPU_STATE_INTERRUPT, (derive_t)cpuinfo[i][CP_INTR],
              now);
  }
  /* }}} #endif HAVE_SYSCTL_KERN_CP_TIMES */

#elif defined(HAVE_SYSCTLBYNAME) /* {{{ */
  /* Only on BSD variant */
  long cpuinfo[CPUSTATES];
  size_t cpuinfo_size;

  cpuinfo_size = sizeof(cpuinfo);

  if (sysctlbyname("kern.cp_time", &cpuinfo, &cpuinfo_size, NULL, 0) < 0) {
    ERROR("cpu plugin: sysctlbyname failed: %s.", STRERRNO);
    return -1;
  }

  cpu_stage(0, COLLECTD_CPU_STATE_USER, (derive_t)cpuinfo[CP_USER], now);
  cpu_stage(0, COLLECTD_CPU_STATE_NICE, (derive_t)cpuinfo[CP_NICE], now);
  cpu_stage(0, COLLECTD_CPU_STATE_SYSTEM, (derive_t)cpuinfo[CP_SYS], now);
  cpu_stage(0, COLLECTD_CPU_STATE_IDLE, (derive_t)cpuinfo[CP_IDLE], now);
  cpu_stage(0, COLLECTD_CPU_STATE_INTERRUPT, (derive_t)cpuinfo[CP_INTR], now);
  /* }}} #endif HAVE_SYSCTLBYNAME */

#elif defined(HAVE_LIBSTATGRAB) /* {{{ */
  sg_cpu_stats *cs;
  cs = sg_get_cpu_stats();

  if (cs == NULL) {
    ERROR("cpu plugin: sg_get_cpu_stats failed.");
    return -1;
  }

  cpu_state(0, COLLECTD_CPU_STATE_IDLE, (derive_t)cs->idle);
  cpu_state(0, COLLECTD_CPU_STATE_NICE, (derive_t)cs->nice);
  cpu_state(0, COLLECTD_CPU_STATE_SWAP, (derive_t)cs->swap);
  cpu_state(0, COLLECTD_CPU_STATE_SYSTEM, (derive_t)cs->kernel);
  cpu_state(0, COLLECTD_CPU_STATE_USER, (derive_t)cs->user);
  cpu_state(0, COLLECTD_CPU_STATE_WAIT, (derive_t)cs->iowait);
  /* }}} #endif HAVE_LIBSTATGRAB */

#elif defined(HAVE_PERFSTAT) /* {{{ */
  perfstat_id_t id;
  int cpus;

  numcpu = perfstat_cpu(NULL, NULL, sizeof(perfstat_cpu_t), 0);
  if (numcpu == -1) {
    WARNING("cpu plugin: perfstat_cpu: %s", STRERRNO);
    return -1;
  }

  if (pnumcpu != numcpu || perfcpu == NULL) {
    free(perfcpu);
    perfcpu = malloc(numcpu * sizeof(perfstat_cpu_t));
  }
  pnumcpu = numcpu;

  id.name[0] = '\0';
  if ((cpus = perfstat_cpu(&id, perfcpu, sizeof(perfstat_cpu_t), numcpu)) < 0) {
    WARNING("cpu plugin: perfstat_cpu: %s", STRERRNO);
    return -1;
  }

  for (int i = 0; i < cpus; i++) {
    cpu_stage(i, COLLECTD_CPU_STATE_IDLE, (derive_t)perfcpu[i].idle, now);
    cpu_stage(i, COLLECTD_CPU_STATE_SYSTEM, (derive_t)perfcpu[i].sys, now);
    cpu_stage(i, COLLECTD_CPU_STATE_USER, (derive_t)perfcpu[i].user, now);
    cpu_stage(i, COLLECTD_CPU_STATE_WAIT, (derive_t)perfcpu[i].wait, now);
  }
#endif                       /* }}} HAVE_PERFSTAT */

  cpu_commit();
  cpu_reset();
  return 0;
}

void module_register(void) {
  plugin_register_init("cpu", init);
  plugin_register_config("cpu", cpu_config, config_keys, config_keys_num);
  plugin_register_read("cpu", cpu_read);
} /* void module_register */
