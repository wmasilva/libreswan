/* information about connections between hosts
 *
 * Copyright (C) 1998-2002,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2007 Ken Bantoft <ken@xelerance.com>
 * Copyright (C) 2008-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2011 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2021 Paul Wouters <paul.wouters@aiven.io>
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
 *
 */

#include "defs.h"
#include "connections.h"
#include "pending.h"
#include "routing.h"		/* for connection_unroute(c) */
#include "log.h"
#include "ip_info.h"
#include "hash_table.h"
#include "iface.h"
#include "orient.h"
#include "host_pair.h"

/*
 * Table of host_pairs (local->remote endpoints/addresses).
 */

const char host_pair_magic[] = "host pair magic";

static size_t jam_host_pair(struct jambuf *buf, const struct host_pair *hp)
{
	size_t s = 0;
	passert(hp->magic == host_pair_magic);
	s += jam_address(buf, &hp->local);
	s += jam(buf, "->");
	s += jam_address(buf, &hp->remote);
	return s;
}

static hash_t hp_hasher(const ip_address local, const ip_address remote)
{
	hash_t hash = zero_hash;
	if (pexpect(address_is_specified(local))) {
		hash = hash_hunk(address_as_shunk(&local), hash);
	}
	if (address_is_specified(remote)) {
		hash = hash_hunk(address_as_shunk(&remote), hash);
	}
	return hash;
}

static hash_t hash_host_pair_addresses(const struct host_pair *hp)
{
	return hp_hasher(hp->local, hp->remote);
}

HASH_TABLE(host_pair, addresses, , STATE_TABLE_SIZE);

#define LIST_RM(ENEXT, E, EHEAD)					\
	{								\
		bool found_ = false;					\
		for (typeof(*(EHEAD)) **ep_ = &(EHEAD);			\
		     *ep_ != NULL; ep_ = &(*ep_)->ENEXT) {		\
			if (*ep_ == (E)) {				\
				*ep_ = (E)->ENEXT;			\
				found_ = true;				\
				break;					\
			}						\
		}							\
		/* we must not come up empty-handed? */			\
		pexpect(found_);					\
	}

/* struct host_pair: a nexus of information about a pair of hosts.
 * A host is an IP address, UDP port pair.  This is a debatable choice:
 * - should port be considered (no choice of port in standard)?
 * - should ID be considered (hard because not always known)?
 * - should IP address matter on our end (we don't know our end)?
 * Only oriented connections are registered.
 * Unoriented connections are kept on the unoriented_connections
 * linked list (using hp_next).  For them, host_pair is NULL.
 */

static struct connection *unoriented_connections = NULL;

/*
 * Returns a host-pair based upon addresses.
 *
 * REMOTE can either be a valid address or UNSET_ADDRESS.
 */

static bool host_pair_matches_addresses(const struct host_pair *hp,
					const ip_address local,
					const ip_address remote)
{
	if (!address_eq_address(hp->local, local)) {
		address_buf lb, rb;
		connection_buf cb;
		dbg("  host_pair: skipping %s->%s, local does not match "PRI_CONNECTION,
		    str_address(&local, &lb), str_address(&remote, &rb),
		    pri_connection(hp->connections, &cb));
		return false;
	}

	/*
	 * XXX: don't assume unset==unset and/or unset==%any, but can
	 * assume IP!={unset,%any).
	 */

	if (address_is_specified(remote) &&
	    !address_eq_address(remote, hp->remote)) {
		connection_buf cb;
		address_buf lb, rb;
		dbg("  host_pair: skipping %s->%s, remote does not match "PRI_CONNECTION,
		    str_address(&local, &lb), str_address(&remote, &rb),
		    pri_connection(hp->connections, &cb));
		return false;
	}

	if (!address_is_specified(remote) &&
	    address_is_specified(hp->remote)) {
		connection_buf cb;
		address_buf lb, rb;
		dbg("  host_pair: skipping %s->%s, unspecified remote does not match "PRI_CONNECTION,
		    str_address(&local, &lb), str_address(&remote, &rb),
		    pri_connection(hp->connections, &cb));
		return false;
	}

	return true;
}

static struct host_pair *alloc_host_pair(ip_address local, ip_address remote, where_t where)
{
	struct host_pair *hp = alloc_thing(struct host_pair, "host pair");
	dbg_alloc("hp", hp, where);
	hp->magic = host_pair_magic;
	hp->local = local;
	/*
	 * Force unset/NULL to 'any' a.k.a. zero; so hash is
	 * consistent and comparisons work.
	 */
	hp->remote = (address_is_unset(&remote) ? address_type(&local)->address.unspec : remote);
	init_hash_table_entry(&host_pair_addresses_hash_table, hp);
	add_hash_table_entry(&host_pair_addresses_hash_table, hp);
	return hp;
}

static void free_unused_host_pair(struct host_pair **hp, where_t where)
{
	if ((*hp)->connections == NULL) {
		del_hash_table_entry(&host_pair_addresses_hash_table, *hp);
		dbg_free("hp", *hp, where);
		pfree(*hp);
		*hp = NULL;
	}
}

struct connection *next_host_pair_connection(const ip_address local,
					     const ip_address remote,
					     struct connection **next,
					     bool first,
					     where_t where)
{
	struct connection *c;
	if (first) {
		address_buf lb, rb;
		dbg("FOR_EACH_HOST_PAIR_CONNECTION(%s->%s) in "PRI_WHERE,
		    str_address(&remote, &lb), str_address(&local, &rb),
		    pri_where(where));
		/*
		 * Find the host-pair list that contains all
		 * connections matching REMOTE->LOCAL.
		 */
		hash_t hash = hp_hasher(local, remote);
		struct list_head *bucket = hash_table_bucket(&host_pair_addresses_hash_table, hash);
		struct host_pair *hp = NULL;
		FOR_EACH_LIST_ENTRY_NEW2OLD(hp, bucket) {
			if (host_pair_matches_addresses(hp, local, remote)) {
				connection_buf cb;
				address_buf lb, rb;
				dbg("  host_pair: %s->%s matches "PRI_CONNECTION,
				    str_address(&remote, &rb), str_address(&local, &lb),
				    pri_connection(hp->connections, &cb));
				break;
			}
		}
		c = (hp != NULL) ? hp->connections : NULL;
	} else {
		c = *next;
	}
	*next = (c != NULL) ? c->hp_next : NULL;
	return c;
}

void connect_to_oriented(struct connection *c)
{
	PASSERT(c->logger, oriented(c));
	ip_address local = c->local->host.addr;
	/* remote could be unset OR any */
	ip_address remote = c->remote->host.addr;
	address_buf lb, rb;
	dbg("looking for host pair matching %s->%s",
	    str_address(&remote, &rb), str_address(&local, &lb));
	hash_t hash = hp_hasher(local, remote);
	struct host_pair *hp = NULL;
	struct list_head *bucket = hash_table_bucket(&host_pair_addresses_hash_table, hash);
	FOR_EACH_LIST_ENTRY_NEW2OLD(hp, bucket) {
		if (host_pair_matches_addresses(hp, local, remote)) {
			break;
		}
	}
	if (hp == NULL) {
		/* no suitable host_pair -- build one */
		ip_address local = c->local->host.addr;
		/* remote could be unset OR any */
		ip_address remote = c->remote->host.addr;
		hp = alloc_host_pair(local, remote, HERE);
	}
	c->host_pair = hp;
	c->hp_next = hp->connections;
	hp->connections = c;
}

void connect_to_unoriented(struct connection *c)
{
	PASSERT(c->logger, !oriented(c));
	/* since this connection isn't oriented, we place it
	 * in the unoriented_connections list instead.
	 */
	pexpect(c->host_pair == NULL);
	pexpect(c->iface == NULL);
	c->host_pair = NULL;
	c->hp_next = unoriented_connections;
	unoriented_connections = c;
}

void delete_oriented_hp(struct connection *c)
{
	struct host_pair *hp = c->host_pair;

	pexpect(c->host_pair != NULL);
	pexpect(c->iface != NULL);

	LIST_RM(hp_next, c, hp->connections);

	pexpect(c->host_pair != NULL);
	c->host_pair = NULL;

	/*
	 * If there are no more connections with this host_pair and we
	 * haven't even made an initial contact, let's delete this guy
	 * in case we were created by an attempted DOS attack.
	 */
	free_unused_host_pair(&hp, HERE);
}

void host_pair_remove_connection(struct connection *c, bool connection_valid)
{
	if (c->host_pair == NULL) {
		delete_unoriented_hp(c, connection_valid);
	} else {
		delete_oriented_hp(c);
	}
}

void delete_unoriented_hp(struct connection *c, bool connection_valid)
{
	/*
	 * When CONNECTION_VALID expect to find/remove C from
	 * the unoriented list.
	 */
	for (struct connection **ep = &unoriented_connections;
	     (*ep) != NULL; ep = &(*ep)->hp_next) {
		if ((*ep) == c) {
			(*ep) = c->hp_next;
			return;
		}
	}
	/* we must not come up empty-handed? */
	PEXPECT(c->logger, !connection_valid);
}

/* update the host pairs with the latest DNS ip address */
void update_host_pairs(struct connection *c)
{
	struct host_pair *hp = c->host_pair;
	const char *dnshostname = c->config->dnshostname;

	/* ??? perhaps we should return early if dnshostname == NULL */

	if (hp == NULL)
		return;

	struct connection *d = hp->connections;

	/* ??? looks as if addr_family is not allowed to change.  Bug? */
	/* ??? why are we using d->config->dnshostname instead of (c->)dnshostname? */
	/* ??? code used to test for d == NULL, but that seems impossible. */

	pexpect(dnshostname == d->config->dnshostname || streq(dnshostname, d->config->dnshostname));

	ip_address new_addr;

	if (d->config->dnshostname == NULL ||
	    ttoaddress_dns(shunk1(d->config->dnshostname),
			      address_type(&d->remote->host.addr), &new_addr) != NULL ||
	    sameaddr(&new_addr, &hp->remote))
		return;

	struct connection *conn_list = NULL;

	while (d != NULL) {
		struct connection *nxt = d->hp_next;

		/*
		 * ??? this test used to assume that dnshostname != NULL
		 * if d->config->dnshostname != NULL.  Is that true?
		 */
		if (d->config->dnshostname != NULL && dnshostname != NULL &&
		    streq(d->config->dnshostname, dnshostname)) {
			/*
			 * If there is a dnshostname and it is the same as
			 * the one that has changed, then change
			 * the connection's remote host address and remove
			 * the connection from the host pair.
			 */

			/*
			 * Unroute the old connection before changing the ip
			 * address.
			 */
			connection_unroute(d, HERE);

			/*
			 * If the client is the peer, also update the
			 * client info
			 */
			if (!d->remote->child.has_client) {
				update_first_selector(d, remote, selector_from_address(new_addr));
				spd_route_db_rehash_remote_client(d->spd);
			}

			d->remote->host.addr = new_addr;
			LIST_RM(hp_next, d, d->host_pair->connections);

			d->hp_next = conn_list;
			conn_list = d;
		}
		d = nxt;
	}

	while (conn_list != NULL) {
		struct connection *nxt = conn_list->hp_next;

		/*
		 * assumption: orientation is the same as before.
		 *
		 * Put C back on unoriented, or add to a host pair.
		 */
		if (oriented(c)) {
			connect_to_oriented(c);
		} else {
			connect_to_unoriented(c);
		}
		conn_list = nxt;
	}

	free_unused_host_pair(&hp, HERE);
}

/*
 * Adjust orientations of connections to reflect newly added
 * interfaces.
 */

void check_orientations(struct logger *logger)
{
	/*
	 * Try to orient unoriented connections by re-building the
	 * unoriented connections list.
	 *
	 * The list is emptied, then as each connection fails to
	 * orient it goes back on the list.
	 */
	dbg("FOR_EACH_UNORIENTED_CONNECTION_... in %s", __func__);
	struct connection *c = unoriented_connections;
	unoriented_connections = NULL;
	while (c != NULL) {
		/* step off */
		struct connection *nxt = c->hp_next;
		bool oriented = orient(&c, logger);
		/*
		 * Put C back on either the unoriented or oriented
		 * list.
		 */
		if (oriented) {
			connect_to_oriented(c);
		} else {
			connect_to_unoriented(c);
		}
		c = nxt;
	}

	/*
	 * Check that no oriented connection has become double-oriented.
	 * In other words, the far side must not match one of our new
	 * interfaces.
	 */
	for (struct iface *iface = next_iface(NULL);
	     iface != NULL; iface = next_iface(iface)) {

		if (iface->ifd_change != IFD_ADD) {
			continue;
		}

		for (unsigned u = 0; u < host_pair_addresses_hash_table.nr_slots; u++) {
			struct list_head *bucket = &host_pair_addresses_hash_table.slots[u];
			struct host_pair *hp = NULL;
			FOR_EACH_LIST_ENTRY_NEW2OLD(hp, bucket) {
				/*
				 * XXX: what's with the maybe compare
				 * the port logic?
				 */
				if (!address_eq_address(hp->remote, iface->local_address)) {
					continue;
				}

				/*
				 * bad news: the whole chain of
				 * connections hanging off this host
				 * pair has both sides matching an
				 * interface.  We'll get rid of them,
				 * using orient and
				 * connect_to_host_pair.
				 */
				struct connection *c = hp->connections;
				hp->connections = NULL;
				while (c != NULL) {
					struct connection *nxt =
						c->hp_next;
					iface_delref(&c->iface);
					c->host_pair = NULL;
					c->hp_next = NULL;
					bool oriented = orient(&c, logger);
					/*
					 * Put C back on either the
					 * unoriented or oriented
					 * list.
					 */
					if (oriented) {
						connect_to_oriented(c);
					} else {
						connect_to_unoriented(c);
					}
					c = nxt;
				}
				/*
				 * XXX: is this ever not the case?
				 */
				free_unused_host_pair(&hp, HERE);
			}
		}
	}
}

void host_pair_db_init(struct logger *logger)
{
	init_hash_table(&host_pair_addresses_hash_table, logger);
}
