/* pfkey interface to the NetBSD/FreeBSD/OSX IPsec mechanism
 *
 * based upon kernel_klips.c.
 *
 * Copyright (C) 2006 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2019 Paul Wouters <pwouters@redhat.com>
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

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/queue.h>		/* for TAILQ_xx macros */

#include <net/pfkeyv2.h>
#include <netinet/in.h>
#include <netipsec/ipsec.h>
#include "libbsdpfkey/libpfkey.h"

#include "sysdep.h"
#include "constants.h"

#include "defs.h"
#include "id.h"
#include "connections.h"
#include "state.h"
#include "kernel.h"
#include "timer.h"
#include "log.h"
#include "whack.h"      /* for RC_LOG_SERIOUS */
#include "packet.h"     /* for pb_stream in nat_traversal.h */
#include "nat_traversal.h"
#include "kernel_alg.h"
#include "kernel_sadb.h"
#include "iface.h"
#include "ip_sockaddr.h"

int pfkeyfd = NULL_FD;
unsigned int pfkey_seq = 1;
bool nat_traversal_support_port_floating;


typedef struct pfkey_item {
	TAILQ_ENTRY(pfkey_item) list;
	struct sadb_msg        *msg;
} pfkey_item;

TAILQ_HEAD(, pfkey_item) pfkey_iq;

static void bsdkame_init_pfkey(struct logger *logger)
{
	/* open PF_KEY socket */

	TAILQ_INIT(&pfkey_iq);

	pfkeyfd = pfkey_open();
	if (pfkeyfd < 0) {
		fatal_errno(PLUTO_EXIT_KERNEL_FAIL, logger, errno,
			    "socket() in init_pfkeyfd()");
	}

	dbg("listening for PF_KEY_V2 on file descriptor %d", pfkeyfd);

	/* probe to see if it is alive */
	if (pfkey_send_register(pfkeyfd, SADB_SATYPE_UNSPEC) < 0 ||
	    pfkey_recv_register(pfkeyfd) < 0) {
		fatal_errno(PLUTO_EXIT_KERNEL_FAIL, logger, errno,
			    "pfkey probe failed");
	}
}

static void bsdkame_process_raw_ifaces(struct raw_iface *rifaces,
				       struct logger *logger)
{
	struct raw_iface *ifp;

	/*
	 * There are no virtual interfaces, so all interfaces are valid
	 */
	for (ifp = rifaces; ifp != NULL; ifp = ifp->next) {
		bool after = false; /* has vfp passed ifp on the list? */
		bool bad = false;
		struct raw_iface *vfp;

		for (vfp = rifaces; vfp != NULL; vfp = vfp->next) {
			if (vfp == ifp) {
				after = true;
			} else if (sameaddr(&ifp->addr, &vfp->addr)) {
				if (after) {
					ipstr_buf b;

					llog(RC_LOG_SERIOUS, logger,
					            "IP interfaces %s and %s share address %s!",
					       ifp->name, vfp->name,
					       ipstr(&ifp->addr, &b));
				}
				bad = true;
			}
		}

		if (bad)
			continue;

		/*
		 * We've got all we need; see if this is a new thing:
		 * search old interfaces list.
		 */
		add_or_keep_iface_dev(ifp, logger);
	}

	/* delete the raw interfaces list */
	while (rifaces != NULL) {
		struct raw_iface *t = rifaces;

		rifaces = t->next;
		pfree(t);
	}
}

static void bsdkame_algregister(int satype, int supp_exttype,
				struct sadb_alg *alg)
{
	switch (satype) {

	case SADB_SATYPE_AH:
		kernel_add_sadb_alg(satype, supp_exttype, alg);
		dbg("algregister_ah(%p) exttype=%d alg_id=%d, alg_ivlen=%d, alg_minbits=%d, alg_maxbits=%d",
		    alg, supp_exttype,
		    alg->sadb_alg_id,
		    alg->sadb_alg_ivlen,
		    alg->sadb_alg_minbits,
		    alg->sadb_alg_maxbits);
		break;

	case SADB_SATYPE_ESP:
		kernel_add_sadb_alg(satype, supp_exttype, alg);
		dbg("algregister(%p) alg_id=%d, alg_ivlen=%d, alg_minbits=%d, alg_maxbits=%d",
		    alg,
		    alg->sadb_alg_id,
		    alg->sadb_alg_ivlen,
		    alg->sadb_alg_minbits,
		    alg->sadb_alg_maxbits);
		break;

	case SADB_X_SATYPE_IPCOMP:
		dbg("ipcomp algregister(%p) alg_id=%d, alg_ivlen=%d, alg_minbits=%d, alg_maxbits=%d",
		    alg,
		    alg->sadb_alg_id,
		    alg->sadb_alg_ivlen,
		    alg->sadb_alg_minbits,
		    alg->sadb_alg_maxbits);
		can_do_IPcomp = true;
		break;

	default:
		break;
	}
}

static void bsdkame_pfkey_register(void)
{
	DBG_log("pfkey_register AH");
	pfkey_send_register(pfkeyfd, SADB_SATYPE_AH);
	pfkey_recv_register(pfkeyfd);

	DBG_log("pfkey_register ESP");
	pfkey_send_register(pfkeyfd, SADB_SATYPE_ESP);
	pfkey_recv_register(pfkeyfd);

	can_do_IPcomp = false; /* It can probably do it - we just need to check out how */
	pfkey_send_register(pfkeyfd, SADB_X_SATYPE_IPCOMP);
	pfkey_recv_register(pfkeyfd);

	foreach_supported_alg(bsdkame_algregister);
}

static void bsdkame_pfkey_register_response(struct sadb_msg *msg)
{
	pfkey_set_supported(msg, msg->sadb_msg_len);
}

static void bsdkame_pfkey_acquire(struct sadb_msg *msg UNUSED)
{
	DBG_log("received acquire --- discarded");
}

/* process a pfkey message */
static void bsdkame_pfkey_async(struct sadb_msg *reply)
{
	switch (reply->sadb_msg_type) {
	case SADB_REGISTER:
		bsdkame_pfkey_register_response(reply);
		break;

	case SADB_ACQUIRE:
		bsdkame_pfkey_acquire(reply);
		break;

	/* case SADB_NAT_T UPDATE STUFF */

	default:
		break;
	}

}

/* asynchronous messages from our queue */
static void bsdkame_dequeue(void)
{
	struct pfkey_item *pi, *pinext;

	for (pi = pfkey_iq.tqh_first; pi; pi = pinext) {
		pinext = pi->list.tqe_next;
		TAILQ_REMOVE(&pfkey_iq, pi, list);

		bsdkame_pfkey_async(pi->msg);
		free(pi->msg);	/* was malloced by pfkey_recv() */
		pfree(pi);
	}
}

/* asynchronous messages directly from PF_KEY socket */
static void bsdkame_process_msg(int i UNUSED, struct logger *unused_logger UNUSED)
{
	struct sadb_msg *reply = pfkey_recv(pfkeyfd);

	bsdkame_pfkey_async(reply);
	free(reply);	/* was malloced by pfkey_recv() */
}

static void bsdkame_consume_pfkey(int pfkeyfd, unsigned int pfkey_seq)
{
	struct sadb_msg *reply = pfkey_recv(pfkeyfd);

	while (reply != NULL && reply->sadb_msg_seq != pfkey_seq) {
		struct pfkey_item *pi;
		pi = alloc_thing(struct pfkey_item, "pfkey item");

		pi->msg = reply;
		TAILQ_INSERT_TAIL(&pfkey_iq, pi, list);

		reply = pfkey_recv(pfkeyfd);
	}
}

/*
 * We are were to install a set of policy, when there is in fact an SA
 * that is already setup.
 *
 * Well, the code is actually shared with shunt_policy(), since for
 * KAME, we set up the policy in an abstracted sense.
 *
 */
static bool bsdkame_raw_policy(enum kernel_policy_op sadb_op,
			       enum what_about_inbound what_about_inbound UNUSED,
			       const ip_address *src_host,
			       const ip_selector *src_client,
			       const ip_address *dst_host,
			       const ip_selector *dst_client,
			       ipsec_spi_t new_spi,
			       enum eroute_type esatype UNUSED,
			       const struct kernel_encap *encap,
			       deltatime_t use_lifetime UNUSED,
			       uint32_t sa_priority UNUSED,
			       const struct sa_marks *sa_marks UNUSED,
			       const uint32_t xfrm_if_id UNUSED,
			       const shunk_t policy_label UNUSED,
			       struct logger *logger)
{
	unsigned int transport_proto = src_client->ipproto;
	pexoect(dst_client->ipproto == transport_proto);

	ip_sockaddr saddr = sockaddr_from_address(selector_prefix(*src_client));
	ip_sockaddr daddr = sockaddr_from_address(selector_prefix(*dst_client));
	char pbuf[512];
	struct sadb_x_policy *policy_struct = (struct sadb_x_policy *)pbuf;
	struct sadb_x_ipsecrequest *ir;
	int policylen;
	int ret;

	int policy = IPSEC_POLICY_IPSEC;

	switch (esatype) {
	case ET_UNSPEC:
	case ET_AH:
	case ET_ESP:
	case ET_IPCOMP:
	case ET_IPIP:
		break;

	case ET_INT:
		/* shunt route */
		switch (ntohl(new_spi)) {
		case SPI_PASS:
			dbg("netlink_raw_policy: SPI_PASS");
			policy = IPSEC_POLICY_NONE;
			break;
		case SPI_HOLD:
			/*
			 * We don't know how to implement %hold, but it is okay.
			 * When we need a hold, the kernel XFRM acquire state
			 * will do the job (by dropping or holding the packet)
			 * until this entry expires. See /proc/sys/net/core/xfrm_acq_expires
			 * After expiration, the underlying policy causing the original acquire
			 * will fire again, dropping further packets.
			 */
			dbg("netlink_raw_policy: SPI_HOLD implemented as no-op");
			return true; /* yes really */
		case SPI_DROP:
		case SPI_REJECT:
		case 0: /* used with type=passthrough - can it not use SPI_PASS ?? */
			policy = IPSEC_POLICY_DISCARD;
			break;
		case SPI_TRAP:
			if (sadb_op == KP_ADD_INBOUND ||
				sadb_op == KP_DELETE_INBOUND)
				return true;

			break;
		case SPI_IGNORE:
		case SPI_TRAPSUBNET: /* unused in our code */
		default:
			bad_case(ntohl(new_spi));
		}
		break;

	default:
		bad_case(esatype);
	}

	const int dir = ((sadb_op == KP_ADD_INBOUND || sadb_op == KP_DELETE_INBOUND) ?
			 IPSEC_DIR_INBOUND : IPSEC_DIR_OUTBOUND);

	/*
	 * XXX: Hack: don't install an inbound spdb entry when tunnel
	 * mode?
	 */
	if (dir == IPSEC_DIR_INBOUND &&
	    encap != NULL && encap->mode == ENCAP_MODE_TRANSPORT) {
		dbg("%s() ignoring inbound non-tunnel %d eroute entry",
		    __func__, esatype);
		return true;
	}

	zero(&pbuf);	/* OK: no pointer fields */

	passert(policy != -1);

	policy_struct->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	policy_struct->sadb_x_policy_type = policy;
	policy_struct->sadb_x_policy_dir  = dir;
	policy_struct->sadb_x_policy_id   = 0; /* needs to be set, and recorded */

	policylen = sizeof(*policy_struct);

	if (policy == IPSEC_POLICY_IPSEC && encap != NULL) {
		ip_sockaddr local_sa = sockaddr_from_address(*src_host);
		ip_sockaddr remote_sa = sockaddr_from_address(*dst_host);

		ir = (struct sadb_x_ipsecrequest *)&policy_struct[1];

		ir->sadb_x_ipsecrequest_len = (sizeof(struct sadb_x_ipsecrequest) +
					       local_sa.len + remote_sa.len);
		/*pexpect(encap != NULL)?*/
		ir->sadb_x_ipsecrequest_mode = (encap->mode == ENCAP_MODE_TUNNEL ? IPSEC_MODE_TUNNEL :
						encap->mode == ENCAP_MODE_TRANSPORT ? IPSEC_MODE_TRANSPORT :
						0);
		ir->sadb_x_ipsecrequest_proto = encap->rule[0].proto;
		dbg("%s() sadb mode %d proto %d",
		    __func__, ir->sadb_x_ipsecrequest_mode, ir->sadb_x_ipsecrequest_proto);
		ir->sadb_x_ipsecrequest_level = IPSEC_LEVEL_REQUIRE;
		ir->sadb_x_ipsecrequest_reqid = 0; /* not used for now */

		uint8_t *addrmem = (uint8_t*)&ir[1];
		memcpy(addrmem, &local_sa.sa,  local_sa.len);
		addrmem += local_sa.len;
		memcpy(addrmem, &remote_sa.sa, remote_sa.len);
		addrmem += remote_sa.len;

		policylen += ir->sadb_x_ipsecrequest_len;

		dbg("request_len=%u policylen=%u",
		    ir->sadb_x_ipsecrequest_len, policylen);
	} else {
		dbg("setting policy=%d", policy);
	}

	policy_struct->sadb_x_policy_len = PFKEY_UNIT64(policylen);

	pfkey_seq++;

	dbg("calling pfkey_send_spdadd() from %s", __func__);
	ret = pfkey_send_spdadd(pfkeyfd,
				&saddr.sa.sa, src_client->maskbits,
				&daddr.sa.sa, dst_client->maskbits,
				transport_proto ? transport_proto : 255 /* proto */,
				(caddr_t)policy_struct, policylen,
				pfkey_seq);

	dbg("consuming pfkey from %s", __func__);
	bsdkame_consume_pfkey(pfkeyfd, pfkey_seq);

	if (ret < 0) {
		selector_buf s, d;
		llog(RC_LOG, logger,
		     "ret = %d from send_spdadd: %s addr=%s/%s seq=%u opname=eroute", ret,
		     ipsec_strerror(),
		     str_selector_subnet_port(src_client, &s),
		     str_selector_subnet_port(dst_client, &d),
		     pfkey_seq);
		return false;
	}
	return true;
}

/* Add/replace/delete a shunt eroute.
 * Such an eroute determines the fate of packets without the use
 * of any SAs.  These are defaults, in effect.
 * If a negotiation has not been attempted, use %trap.
 * If negotiation has failed, the choice between %trap/%pass/%drop/%reject
 * is specified in the policy of connection c.
 */
static bool bsdkame_shunt_policy(enum kernel_policy_op op,
				 enum what_about_inbound what_about_bound UNUSED,
				 const struct connection *c,
				 const struct spd_route *sr,
				 enum routing_t rt_kind,
				 shunk_t sec_label,
				 const char *opname,
				 struct logger *logger)
{
	enum shunt_policy shunt_policy =
		(rt_kind == RT_ROUTED_PROSPECTIVE ? c->config->prospective_shunt :
		 c->config->failure_shunt);
	int policy = -1;

	switch (shunt_policy) {
	case SHUNT_NONE:
		/* we're supposed to end up with no eroute: rejig op and opname */
		switch (op) {
		case KP_REPLACE_OUTBOUND:
			/* replace with nothing == delete */
			op = KP_DELETE_OUTBOUND;
			opname = "delete";
			break;
		case KP_ADD_OUTBOUND:
			/* add nothing == do nothing */
			return true;

		case KP_DELETE_OUTBOUND:
			/* delete remains delete */
			break;

		case KP_ADD_INBOUND:
			break;

		case KP_DELETE_INBOUND:
			break;

		default:
			bad_case(op);
		}
		break;

	case SHUNT_TRAP:
		policy = IPSEC_POLICY_IPSEC;
		break;

	case SHUNT_PASS:
		policy = IPSEC_POLICY_NONE; /* BYPASS is for sockets only */
		break;

	case SHUNT_REJECT:
	case SHUNT_DROP:
		policy = IPSEC_POLICY_DISCARD;
		break;

	default:
		dbg("shunt_policy() called with policy=%08x", shunt_policy);
	}

	if (sr->routing == RT_ROUTED_ECLIPSED && c->kind == CK_TEMPLATE) {
		/* We think that we have an eroute, but we don't.
		 * Adjust the request and account for eclipses.
		 */
		passert(eclipsable(sr));
		switch (op) {
		case KP_REPLACE_OUTBOUND:
			/* really an add */
			op = KP_ADD_OUTBOUND;
			opname = "replace eclipsed";
			eclipse_count--;
			break;

		case KP_DELETE_OUTBOUND:
			/* delete unnecessary: we don't actually have an eroute */
			eclipse_count--;
			return true;

		case KP_ADD_OUTBOUND:
		default:
			bad_case(op);
		}
	} else if (eclipse_count > 0 && op == KP_DELETE_OUTBOUND && eclipsable(sr)) {
		/* maybe we are uneclipsing something */
		struct spd_route *esr;
		const struct connection *ue = eclipsed(c, &esr);

		if (ue != NULL) {
			esr->routing = RT_ROUTED_PROSPECTIVE;
			return bsdkame_shunt_policy(KP_REPLACE_OUTBOUND,
						    THIS_IS_NOT_INBOUND,
						    ue, esr,
						    RT_ROUTED_PROSPECTIVE,
						    sec_label,
						    "restoring eclipsed",
						    logger);
		}
	}

	switch (op) {
	case KP_REPLACE_OUTBOUND:
	case KP_ADD_OUTBOUND:
	{
		const ip_selector mine = sr->this.client;
		const ip_selector peers = sr->that.client;
		ip_sockaddr saddr = sockaddr_from_address_port(selector_prefix(mine),
							       selector_port(mine));
		ip_sockaddr daddr = sockaddr_from_address_port(selector_prefix(peers),
							       selector_port(peers));
		char pbuf[512];
		char buf2[256];
		struct sadb_x_policy *policy_struct =
			(struct sadb_x_policy *)pbuf;
		struct sadb_x_ipsecrequest *ir;
		int policylen;
		int ret;

		snprintf(buf2, sizeof(buf2),
			 "eroute_connection %s", opname);

		zero(&pbuf);	/* OK: no pointer fields */

		passert(policy != -1);

		policy_struct->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
		policy_struct->sadb_x_policy_type = policy;
		policy_struct->sadb_x_policy_dir  = IPSEC_DIR_OUTBOUND;
		policy_struct->sadb_x_policy_id   = 0; /* needs to be set, and recorded */

		policylen = sizeof(*policy_struct);

		if (policy == IPSEC_POLICY_IPSEC) {
			ip_sockaddr local_sa = sockaddr_from_address(sr->this.host_addr);
			ip_sockaddr remote_sa = sockaddr_from_address(sr->that.host_addr);

			ir = (struct sadb_x_ipsecrequest *)&policy_struct[1];

			ir->sadb_x_ipsecrequest_len = (sizeof(struct sadb_x_ipsecrequest) +
						       local_sa.len + remote_sa.len);
			if (c->policy & POLICY_ENCRYPT) {
				/* maybe should look at IPCOMP too */
				ir->sadb_x_ipsecrequest_proto = IPPROTO_ESP;
			} else {
				ir->sadb_x_ipsecrequest_proto = IPPROTO_AH;
			}

			if (c->policy & POLICY_TUNNEL)
				ir->sadb_x_ipsecrequest_mode =
					IPSEC_MODE_TUNNEL;
			else
				ir->sadb_x_ipsecrequest_mode =
					IPSEC_MODE_TRANSPORT;
			ir->sadb_x_ipsecrequest_level = IPSEC_LEVEL_REQUIRE;
			ir->sadb_x_ipsecrequest_reqid = 0; /* not used for now */

			uint8_t *addrmem = (uint8_t *)&ir[1];
			memcpy(addrmem, &local_sa.sa,  local_sa.len);
			addrmem += local_sa.len;
			memcpy(addrmem, &remote_sa.sa, remote_sa.len);
			addrmem += remote_sa.len;

			policylen += ir->sadb_x_ipsecrequest_len;

			DBG_log("request_len=%u policylen=%u",
				ir->sadb_x_ipsecrequest_len, policylen);

		} else {
			DBG_log("setting policy=%d", policy);
		}

		policy_struct->sadb_x_policy_len = PFKEY_UNIT64(policylen);

		pfkey_seq++;
		dbg("calling pfkey_send_spdadd() from %s", __func__);
		ret = pfkey_send_spdadd(pfkeyfd,
					&saddr.sa.sa, selector_prefix_bits(mine),
					&daddr.sa.sa, selector_prefix_bits(peers),
					255 /* proto */,
					(caddr_t)policy_struct, policylen,
					pfkey_seq);

		bsdkame_consume_pfkey(pfkeyfd, pfkey_seq);

		if (ret < 0) {
			selector_buf s, d;
			llog(RC_LOG, logger,
			     "ret = %d from send_spdadd: %s addr=%s/%s seq=%u opname=%s",
			     ret, ipsec_strerror(),
			     str_selector_subnet_port(&mine, &s),
			     str_selector_subnet_port(&peers, &d),
			     pfkey_seq, opname);
			return false;
		}
		return true;
	}

	case KP_DELETE_OUTBOUND:
	{
		/* need to send a delete message */
		const ip_selector mine = sr->this.client;
		const ip_selector peers = sr->that.client;
		ip_sockaddr saddr = sockaddr_from_address_port(selector_prefix(mine),
							       selector_port(mine));
		ip_sockaddr daddr = sockaddr_from_address_port(selector_prefix(peers),
							       selector_port(peers));
		char pbuf[512];
		char buf2[256];
		struct sadb_x_policy *policy_struct =
			(struct sadb_x_policy *)pbuf;
		int policylen;
		int ret;

		DBG_log("need to send a delete message");

		snprintf(buf2, sizeof(buf2),
			 "eroute_connection %s", opname);

		policy_struct->sadb_x_policy_exttype = SADB_X_EXT_POLICY;

		/* this might be wrong! --- probably should use spddelete2() */
		policy_struct->sadb_x_policy_type = IPSEC_POLICY_IPSEC;
		policy_struct->sadb_x_policy_dir  = IPSEC_DIR_OUTBOUND;
		policy_struct->sadb_x_policy_id = 0;

		policylen = sizeof(*policy_struct);

		policy_struct->sadb_x_policy_len = PFKEY_UNIT64(policylen);

		pfkey_seq++;
		dbg("calling pfkey_send_spddelete() from %s", __func__);
		ret = pfkey_send_spddelete(pfkeyfd,
					   &saddr.sa.sa, selector_prefix_bits(mine),
					   &daddr.sa.sa, selector_prefix_bits(peers),
					   255 /* proto */,
					   (caddr_t)policy_struct, policylen,
					   pfkey_seq);

		bsdkame_consume_pfkey(pfkeyfd, pfkey_seq);

		if (ret < 0) {
			selector_buf s, d;
			llog(RC_LOG, logger,
			     "ret = %d from send_spdadd: %s addr=%s/%s seq=%u opname=%s",
			     ret, ipsec_strerror(),
			     str_selector_subnet_port(&mine, &s),
			     str_selector_subnet_port(&peers, &d),
			     pfkey_seq, opname);
			return false;
		}
		return true;

		break;
	}
	case KP_ADD_INBOUND:
	case KP_REPLACE_INBOUND:
	case KP_DELETE_INBOUND:
		bad_case(op);
	}
	return false;
}

static bool bsdkame_add_sa(const struct kernel_sa *sa, bool replace,
			   struct logger *logger)
{
	ip_sockaddr saddr = sockaddr_from_address(*sa->src.address);
	ip_sockaddr daddr = sockaddr_from_address(*sa->dst.address);
	char keymat[256];
	int ret;

	/* only the inner-most SA gets the tunnel flag */
	int mode = (sa->tunnel && sa->level == 0 ? IPSEC_MODE_TUNNEL : IPSEC_MODE_TRANSPORT);

	int satype;
	switch (sa->esatype) {
	case ET_AH:
		satype = SADB_SATYPE_AH;
		break;
	case ET_ESP:
		satype = SADB_SATYPE_ESP;
		break;
	case ET_IPCOMP:
		satype = SADB_X_SATYPE_IPCOMP;
		break;
	case ET_IPIP:
		llog(RC_LOG, logger, "in %s() ignoring nonsensical ET_IPIP", __func__);
		return true;

	default:
	case ET_INT:
	case ET_UNSPEC:
		bad_case(sa->esatype);
	}

	if ((sa->enckeylen + sa->authkeylen) > sizeof(keymat)) {
		llog(RC_LOG, logger,
			    "Key material is too big for kernel interface: %d>%zu",
			    (sa->enckeylen + sa->authkeylen),
			    sizeof(keymat));
		return false;
	}

	pfkey_seq++;

	memcpy(keymat, sa->enckey, sa->enckeylen);
	memcpy(keymat + sa->enckeylen, sa->authkey, sa->authkeylen);

	DBG_dump("keymat", keymat, sa->enckeylen + sa->authkeylen);
	dbg("calling pfkey_send_add2() for pfkeyseq=%d encalg=%s/%d authalg=%s/%d spi=%08x, reqid=%u, satype=%d",
	    pfkey_seq,
	    sa->encrypt->common.fqn, sa->enckeylen,
	    sa->integ->common.fqn, sa->authkeylen,
	    sa->spi, sa->reqid, satype);

	ret =  (replace ? pfkey_send_update : pfkey_send_add)(pfkeyfd,
			    satype, mode,
			    &saddr.sa.sa, &daddr.sa.sa,
			    sa->spi,
			    sa->reqid,  /* reqid */
			    64,         /* wsize, replay window size */
			    keymat,
							      sa->encrypt->encrypt_sadb_ealg_id,
			    sa->enckeylen,
							      sa->integ->integ_sadb_aalg_id,
			    sa->authkeylen,
			    0,                  /*flags */
			    0,                  /* l_alloc */
			    0,                  /* l_bytes */
			    deltasecs(sa->sa_lifetime),    /* l_addtime */
			    0,                  /* l_usetime, */
			    pfkey_seq);
#if 0
	struct pfkey_send_sa_args add_args = {
		.so = pfkeyfd,
		.type = (replace ? SADB_UPDATE : SADB_ADD),
		.satype = satype,
		.mode = mode,
		.src = &saddr.sa.sa,
		.dst = &daddr.sa.sa,
		.spi = sa->spi,
		.reqid = sa->reqid,  /* reqid */
		.wsize = 64,         /* wsize, replay window size */
		.keymat = keymat,
		.e_type = sa->encrypt->encrypt_sadb_ealg_id,
		.e_keylen = sa->enckeylen,
		.a_type = sa->integ->integ_sadb_aalg_id,
		.a_keylen = sa->authkeylen,
		.flags = 0,                  /*flags */
		.l_alloc = 0,                  /* l_alloc */
		.l_bytes = 0,                  /* l_bytes */
		.l_addtime = deltasecs(sa->sa_lifetime),    /* l_addtime */
		.l_usetime = 0,                  /* l_usetime, */
		.seq = pfkey_seq,
	};
	ret = pfkey_send_add2(&add_args);
#endif

	bsdkame_consume_pfkey(pfkeyfd, pfkey_seq);

	if (ret < 0) {
		llog(RC_LOG, logger,
			    "ret = %d from add_sa: %s seq=%d", ret,
			    ipsec_strerror(), pfkey_seq);
		return false;
	}

	return true;
}

static bool bsdkame_del_sa(const struct kernel_sa *sa UNUSED,
				   struct logger *logger UNUSED)
{
	return true;
}

/* Check if there was traffic on given SA during the last idle_max
 * seconds. If TRUE, the SA was idle and DPD exchange should be performed.
 * If FALSE, DPD is not necessary. We also return TRUE for errors, as they
 * could mean that the SA is broken and needs to be replace anyway.
 */
static bool bsdkame_was_eroute_idle(struct state *st UNUSED,
				    deltatime_t idle_max UNUSED)
{
	passert(false);
	return false;
}

static bool bsdkame_except_socket(int socketfd, int family, struct logger *logger)
{
	struct sadb_x_policy policy;
	int level, optname;

	switch (family) {
	case AF_INET:
		level = IPPROTO_IP;
		optname = IP_IPSEC_POLICY;
		break;
#ifdef INET6
	case AF_INET6:
		level = IPPROTO_IPV6;
		optname = IPV6_IPSEC_POLICY;
		break;
#endif
	default:
		llog(RC_LOG, logger, "unsupported address family (%d)", family);
		return false;
	}

	zero(&policy);	/* OK: no pointer fields */
	policy.sadb_x_policy_len = PFKEY_UNIT64(sizeof(policy));
	policy.sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	policy.sadb_x_policy_type = IPSEC_POLICY_BYPASS;
	policy.sadb_x_policy_dir = IPSEC_DIR_INBOUND;
	if (setsockopt(socketfd, level, optname, &policy,
		       sizeof(policy)) == -1) {
		log_errno(logger, errno, "bsdkame except socket setsockopt");
		return false;
	}
	policy.sadb_x_policy_dir = IPSEC_DIR_OUTBOUND;
	if (setsockopt(socketfd, level, optname, &policy,
		       sizeof(policy)) == -1) {
		log_errno(logger, errno, "bsdkame except socket setsockopt");
		return false;
	}
	return true;
}

static bool bsdkame_detect_offload(const struct raw_iface *ifp UNUSED,
				   struct logger *logger UNUSED)
{
	dbg("%s: nothing to do", __func__);
	return false;
}

const struct kernel_ops bsdkame_kernel_ops = {
	.type =USE_BSDKAME,
	.kern_name = "bsdkame",
	.async_fdp = &pfkeyfd,
	.replay_window = 64,
	.pfkey_register = bsdkame_pfkey_register,
	.process_queue = bsdkame_dequeue,
	.process_msg = bsdkame_process_msg,
	.raw_policy = bsdkame_raw_policy,
	.shunt_policy = bsdkame_shunt_policy,
	.add_sa = bsdkame_add_sa,
	.grp_sa = NULL,
	.del_sa = bsdkame_del_sa,
	.get_spi = NULL,
	.eroute_idle = bsdkame_was_eroute_idle,
	.init = bsdkame_init_pfkey,
	.shutdown = NULL,
	.exceptsocket = bsdkame_except_socket,
	.process_raw_ifaces = bsdkame_process_raw_ifaces,
	.overlap_supported = false,
	.sha2_truncbug_support = false,
	.v6holes = NULL,
	.detect_offload = bsdkame_detect_offload,
};
