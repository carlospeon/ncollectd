/**
 * collectd - src/users.c
 * Copyright (C) 2005-2007  Sebastian Harl
 * Copyright (C) 2005       Niki W. Waibel
 * Copyright (C) 2005-2007  Florian octo Forster
 * Copyright (C) 2008       Oleg King
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the license is applicable.
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
 *   Sebastian Harl <sh at tokkee.org>
 *   Niki W. Waibel <niki.waibel at newlogic.com>
 *   Florian octo Forster <octo at collectd.org>
 *   Oleg King <king2 at kaluga.ru>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#if HAVE_STATGRAB_H
#include <statgrab.h>
#endif /* HAVE_STATGRAB_H */

#if HAVE_UTMPX_H
#include <utmpx.h>
/* #endif HAVE_UTMPX_H */

#elif HAVE_UTMP_H
#include <utmp.h>
/* #endif HAVE_UTMP_H */
#endif

static void users_submit(gauge_t value)
{
  metric_family_t fam = {
      .name = "host_users",
      .type = METRIC_TYPE_GAUGE,
      .help = "Number of users currently logged into the system"
  };

  metric_family_metric_append(&fam, (metric_t){
                                        .value.gauge = value,
                                    });

  int status = plugin_dispatch_metric_family(&fam);
  if (status != 0) {
    ERROR("users plugin: plugin_dispatch_metric_family failed: %s",
          STRERROR(status));
  }

  metric_family_metric_reset(&fam);
}

static int users_read(void)
{
#if HAVE_GETUTXENT
  unsigned int users = 0;
  struct utmpx *entry = NULL;

  /* according to the *utent(3) man page none of the functions sets errno
     in case of an error, so we cannot do any error-checking here */
  setutxent();

  while (NULL != (entry = getutxent())) {
    if (USER_PROCESS == entry->ut_type)
      ++users;
  }
  endutxent();

  users_submit(users);
  /* #endif HAVE_GETUTXENT */

#elif HAVE_GETUTENT
  unsigned int users = 0;
  struct utmp *entry = NULL;

  /* according to the *utent(3) man page none of the functions sets errno
     in case of an error, so we cannot do any error-checking here */
  setutent();

  while (NULL != (entry = getutent())) {
    if (USER_PROCESS == entry->ut_type)
      ++users;
  }
  endutent();

  users_submit(users);
  /* #endif HAVE_GETUTENT */

#elif HAVE_LIBSTATGRAB
  sg_user_stats *us;

#if HAVE_LIBSTATGRAB_0_90
  size_t num_entries;
  us = sg_get_user_stats(&num_entries);
#else
  us = sg_get_user_stats();
#endif
  if (us == NULL)
    return -1;

  users_submit((gauge_t)
#if HAVE_LIBSTATGRAB_0_90
                   num_entries);
#else
                   us->num_entries);
#endif
  /* #endif HAVE_LIBSTATGRAB */

#else
#error "No applicable input method."
#endif

  return 0;
}

void module_register(void)
{
  plugin_register_read("users", users_read);
}
