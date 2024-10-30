/* getopt parsing, for libreswan
 *
 * Copyright (C) 2023,2024 Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stdlib.h>		/* for exit() */

#include "optarg.h"

#include "sparse_names.h"
#include "passert.h"
#include "lswlog.h"
#include "ip_info.h"

int long_index;

static const struct logger *optarg_logger;

void optarg_init(const struct logger *logger)
{
	optarg_logger = logger;
}

/*
 * XXX: almost identical code lives in plutomain.c
 */

NEVER_RETURNS
static void fatal_optarg(const char *fmt, ...)
{
	/*
	 * Not exit_pluto() or fatal() as pluto isn't yet up and
	 * running?
	 */
	passert(long_index >= 0);
	const char *optname = long_opts[long_index].name;
	LLOG_JAMBUF(WHACK_STREAM, optarg_logger, buf) {
		if (optarg == NULL) {
			jam(buf, "option --%s invalid: ", optname);
		} else {
			jam(buf, "option --%s '%s' invalid: ", optname, optarg);
		}
		va_list ap;
		va_start(ap, fmt);
		jam_va_list(buf, fmt, ap);
		va_end(ap);
	}
	/* not exit_pluto as pluto isn't yet up and running? */
	exit(PLUTO_EXIT_FAIL);
}

deltatime_t optarg_deltatime(const struct timescale *timescale)
{
	passert((long_opts[long_index].has_arg == required_argument) ||
		(long_opts[long_index].has_arg == optional_argument && optarg != NULL));
	deltatime_t deltatime;
	diag_t diag = ttodeltatime(optarg, &deltatime, timescale);
	if (diag != NULL) {
		fatal_optarg("%s", str_diag(diag));
	}
	return deltatime;
}

uintmax_t optarg_uintmax(void)
{
	passert((long_opts[long_index].has_arg == required_argument) ||
		(long_opts[long_index].has_arg == optional_argument && optarg != NULL));
	uintmax_t val;
	err_t err = shunk_to_uintmax(shunk1(optarg), NULL, /*base*/0, &val);
	if (err != NULL) {
		fatal_optarg("%s", err);
	}
	return val;
}

/*
 * Lookup OPTARG in NAMES.
 *
 * When optional_argument OPTARG is missing, return OPTIONAL (pass
 * optional=0 when required_argument).
 */

uintmax_t optarg_sparse(unsigned optional, const struct sparse_names *names)
{
	if (optarg == NULL) {
		passert(long_opts[long_index].has_arg == optional_argument);
		passert(optional != 0);
		return optional;
	}

	const struct sparse_name *name = sparse_lookup(names, shunk1(optarg));
	if (name == NULL) {
		JAMBUF(buf) {
			jam(buf, "'%s' is not recognised, valid arguments are: ", optarg);
			jam_sparse_names(buf, names, ", ");
			fatal_optarg(PRI_SHUNK, pri_shunk(jambuf_as_shunk(buf)));
		}
	}
	return name->value;
}

/*
 * Addresses.
 */

void optarg_family(struct family *family, const struct ip_info *info)
{
	if (family != NULL && family->type == NULL) {
		family->type = info;
		family->used_by = long_opts[long_index].name;
	}
}

ip_address optarg_address_dns(struct family *family)
{
	ip_address address;
	err_t err = ttoaddress_dns(shunk1(optarg), family->type, &address);
	if (err != NULL) {
		fatal_optarg("%s", err);
	}
	optarg_family(family, address_info(address));
	return address;
}

ip_cidr optarg_cidr_num(struct family *family)
{
	ip_cidr cidr;
	err_t err = ttocidr_num(shunk1(optarg), family->type, &cidr);
	if (err != NULL) {
		fatal_optarg("%s", err);
	}
	optarg_family(family, cidr_info(cidr));
	return cidr;
}

ip_address optarg_any(struct family *family)
{
	optarg_family(family, &ipv4_info);
	return family->type->address.unspec;
}
