/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2013 Google, Inc
 */

#ifndef __DM_UTIL_H
#define __DM_UTIL_H

struct dm_stats;

/*
 * Pick the log level for each helper based on the DM log verbosity choice.
 * A disabled level falls back to LOGL_DEBUG, which is compiled out by
 * default. Each level includes the ones below it, so DM_WARN also enables
 * errors.
 */
#if CONFIG_IS_ENABLED(DM_WARN)
#define _DM_WARN_LEVEL	LOGL_WARNING
#else
#define _DM_WARN_LEVEL	LOGL_DEBUG
#endif

#if CONFIG_IS_ENABLED(DM_WARN) || CONFIG_IS_ENABLED(DM_ERR)
#define _DM_ERR_LEVEL	LOGL_ERR
#else
#define _DM_ERR_LEVEL	LOGL_DEBUG
#endif

#define dm_warn(fmt...) log(LOGC_DM, _DM_WARN_LEVEL, ##fmt)
#define dm_err(fmt...) log(LOGC_DM, _DM_ERR_LEVEL, ##fmt)

struct list_head;

/**
 * Dump out a tree of all devices starting @uclass
 *
 * @dev_name: udevice name
 * @extended: true if forword-matching expected
 * @sort: Sort by uclass name
 */
void dm_dump_tree(char *dev_name, bool extended, bool sort);

/*
 * Dump out a list of uclasses and their devices
 *
 * @uclass: uclass name
 * @extended: true if forword-matching expected
 */
void dm_dump_uclass(char *uclass, bool extended);

#ifdef CONFIG_DEBUG_DEVRES
/* Dump out a list of device resources */
void dm_dump_devres(void);
#else
static inline void dm_dump_devres(void)
{
}
#endif

/* Dump out a list of drivers */
void dm_dump_drivers(void);

/* Dump out a list with each driver's compatibility strings */
void dm_dump_driver_compat(void);

/* Dump out a list of drivers with static platform data */
void dm_dump_static_driver_info(void);

/**
 * dm_dump_mem() - Dump stats on memory usage in driver model
 *
 * @mem: Stats to dump
 */
void dm_dump_mem(struct dm_stats *stats);

#if CONFIG_IS_ENABLED(OF_PLATDATA_INST) && CONFIG_IS_ENABLED(READ_ONLY)
void *dm_priv_to_rw(void *priv);
#else
static inline void *dm_priv_to_rw(void *priv)
{
	return priv;
}
#endif

#endif
