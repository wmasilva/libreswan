/* ip_endpoint tests, for libreswan
 *
 * Copyright (C) 2000  Henry Spencer.
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2018 Andrew Cagney
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/lgpl-2.1.txt>.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 *
 */

#include <stdio.h>
#include <string.h>

#include "lswcdefs.h"		/* for elemsof() */
#include "constants.h"		/* for streq() */
#include "ip_endpoint.h"

#include "ipcheck.h"

static void check_str_endpoint(void)
{
	static const struct test {
		const int family;
		const char *in;
		const char *out;
	} tests[] = {
		/* anything else? */
		{ 4, "1.2.3.4",			"1.2.3.4:65535" },
		{ 4, "255.255.255.255",		"255.255.255.255:65535" },
		{ 6, "1:12:3:14:5:16:7:18",	"[1:12:3:14:5:16:7:18]:65535" },
		{ 6, "11:22:33:44:55:66:77:88",	"[11:22:33:44:55:66:77:88]:65535" },
		/* treat special different ? */
		{ 6, "0:0:0:0:0:0:0:1",		"[::1]:65535" },
		{ 6, "0:0:0:0:0:0:0:0",		"[::]:65535" },
	};

	for (size_t ti = 0; ti < elemsof(tests); ti++) {
		const struct test *t = &tests[ti];
		PRINT_IN(stdout, " -> '%s'", t->out);

		/* convert it *to* internal format */
		ip_address a;
		const struct ip_info *type = NULL;
		err_t err = numeric_to_address(shunk1(t->in), type, &a);
		if (err != NULL) {
			FAIL_IN("numeric_to_address() failed: %s", err);
			continue;
		}
		ip_endpoint e = endpoint(&a, 65535);
		CHECK_TYPE(PRINT_IN, endpoint_type(&e));

		/* now convert it back */
		endpoint_buf buf;
		const char *out = str_endpoint(&e, &buf);
		if (out == NULL) {
			FAIL_IN("failed");
		} else if (!strcaseeq(t->out, out)) {
			FAIL_IN("returned '%s', expected '%s'",
				out, t->out);
		}
	}
}

static void check_sockaddr_as_endpoint(void)
{
	static const struct test {
		const int family;
		const char *in;
		uint8_t addr[16];
		int port;
		size_t size;
		const char *err;
		const char *out;
	} tests[] = {
		{ 4, "1.2.3.4:65535", { 1, 2, 3, 4, }, 65535, sizeof(struct sockaddr_in), },
		{ 6, "[1::1]:65535", { [1] = 1, [15] = 1, }, 65535, sizeof(struct sockaddr_in6), },
		/* far too small */
		{ 4, "1.2.3.4:65535", { 1, 2, 3, 4, }, 65535, 0, "truncated", "<unspecified:>", },
		{ 6, "[1::1]:65535", { [1] = 1, [15] = 1, }, 65535, 0, "truncated", "<unspecified:>", },
		/* somewhat too small */
#define SIZE offsetof(struct sockaddr, sa_family) + sizeof(sa_family_t)
		{ 4, "1.2.3.4:65535", { 1, 2, 3, 4, }, 65535, SIZE, "wrong length", "<unspecified:>", },
		{ 6, "[1::1]:65535", { [1] = 1, [15] = 1, }, 65535, SIZE, "wrong length", "<unspecified:>", },
#undef SIZE
	};

	for (size_t ti = 0; ti < elemsof(tests); ti++) {
		const struct test *t = &tests[ti];
		const char *expect_out = t->out == NULL ? t->in : t->out;
		PRINT_IN(stdout, " -> '%s'", expect_out);

		/* construct a raw sockaddr */
		ip_sockaddr sa = {
			.sa = {
				.sa_family = SA_FAMILY(t->family),
			},
		};
		switch (t->family) {
		case 4:
			memcpy(&sa.sin.sin_addr, t->addr, sizeof(sa.sin.sin_addr));
			sa.sin.sin_port = htons(t->port);
			break;
		case 6:
			memcpy(&sa.sin6.sin6_addr, t->addr, sizeof(sa.sin6.sin6_addr));
			sa.sin6.sin6_port = htons(t->port);
			break;
		}

		/* sockaddr->endpoint */
		ip_endpoint endpoint;
		err_t err = sockaddr_to_endpoint(&sa, t->size, &endpoint);
		if (err != NULL) {
			if (t->err == NULL) {
				FAIL_IN("sockaddr_to_endpoint() unexpectedly failed: %s", err);
			} else if (!streq(err, t->err)) {
				FAIL_IN("sockaddr_to_endpoint() returned error '%s', expecting '%s'", err, t->err);
			}
			if (endpoint_type(&endpoint) != NULL) {
				FAIL_IN("sockaddr_to_endpoint() failed yet endpoint has a type");
			}
		} else if (t->err != NULL) {
			FAIL_IN("sockaddr_to_endpoint() should have failed: %s", t->err);
		} else {
			CHECK_TYPE(PRINT_IN, endpoint_type(&endpoint));
		}

		/* endpoint->sockaddr */
		ip_sockaddr esa;
		size_t size = endpoint_to_sockaddr(&endpoint, &esa);
		if (err == NULL) {
			if (size == 0) {
				FAIL_IN("endpoint_to_sockaddr() returned %zu, expecting non-zero", size);
			} else if (size > sizeof(esa)) {
				FAIL_IN("endpoint_to_sockaddr() returned %zu, expecting %zu or smaller",
					size, sizeof(esa));
			} else if (!memeq(&esa, &sa, sizeof(esa))) {
				/* compare the entire buffer, not just size */
				FAIL_IN("endpoint_to_sockaddr() returned a different value");
			}
		} else {
			if (size != 0) {
				FAIL_IN("endpoint_to_sockaddr() returned %zu, expecting non-zero", size);
			}
		}

		/* as a string */
		endpoint_buf buf;
		const char *out = str_endpoint(&endpoint, &buf);
		if (out == NULL) {
			FAIL_IN("str_endpoint() returned NULL");
		} else if (!strcaseeq(expect_out, out)) {
			FAIL_IN("str_endpoint() returned '%s', expecting '%s'",
				out, expect_out);
		}
	}
}

static void check_endpoint_port(void)
{
	/*
	 * XXX: can't yet do invalid ports.
	 */
	static const struct test {
		int family;
		char *in;
		uint16_t hport;
		uint8_t nport[2];
	} tests[] = {
		/* any */
		{ 4, "0.0.0.0", 0, { 0, 0, }, },
		{ 6, "::0", 0, { 0, 0, }, },
		/* longest */
		{ 4, "101.102.103.104", 65534, { 255, 254, }, },
		{ 6, "1001:1002:1003:1004:1005:1006:1007:1008", 65534, { 255, 254, }, },
	};

	const char *oops;

	for (size_t ti = 0; ti < elemsof(tests); ti++) {
		const struct test *t = &tests[ti];
		PRINT_IN(stdout, " -> '%d'", t->hport);

		const struct ip_info *type = IP_TYPE(t->family);

		ip_address a;
		oops = numeric_to_address(shunk1(t->in), type, &a);
		if (oops != NULL) {
			/* Error occurred, but we didn't expect one  */
			FAIL_IN("ttosubnet failed: %s", oops);
		}
		ip_endpoint e = endpoint(&a, t->hport);

		CHECK_TYPE(PRINT_IN, endpoint_type(&e));

		uint16_t hport = endpoint_hport(&e);
		if (!memeq(&hport, &t->hport, sizeof(hport))) {
			FAIL_IN("endpoint_hport() returned '%d', expected '%d'",
				hport, t->hport);
		}

		uint16_t nport = endpoint_nport(&e);
		if (!memeq(&nport, &t->nport, sizeof(nport))) {
			FAIL_IN("endpoint_nport() returned '%04x', expected '%02x%02x'",
				nport, t->nport[0], t->nport[1]);
		}

		/* tweak the port numbers */
		uint16_t hport_plus_one = t->hport+1;
		uint16_t nport_plus_one = ntohs(t->hport+1);
		/* check math? */
		uint8_t nport_plus_plus[2];
		memcpy(nport_plus_plus, t->nport, sizeof(nport_plus_plus));
		nport_plus_plus[1]++;
		if (!memeq(&nport_plus_one, nport_plus_plus, sizeof(nport_plus_one))) {
			FAIL_IN("can't do basic math");
		}

		/* hport+1 -> nport+1 */
		ip_endpoint hp = e;
		update_endpoint_hport(&hp, hport_plus_one);
		uint16_t nportp = endpoint_nport(&hp);
		if (!memeq(&nportp, &nport_plus_one, sizeof(nportp))) {
			FAIL_IN("endpoint_nport(set_endpoint_hport(+1)) returned '%04x', expected '%04x'",
				nportp, nport_plus_one);
		}

		/* nport+1 -> hport+1 */
		ip_endpoint np = e;
		update_endpoint_nport(&np, nport_plus_one);
		uint16_t hportp = endpoint_hport(&np);
		if (!memeq(&hportp, &hport_plus_one, sizeof(hportp))) {
			FAIL_IN("endpoint_hport(set_endpoint_nport(+1)) returned '%d', expected '%d'",
				hportp, hport_plus_one);
		}
	}
}

void ip_endpoint_check(void)
{
	check_str_endpoint();
	check_sockaddr_as_endpoint();
	check_endpoint_port();
}
