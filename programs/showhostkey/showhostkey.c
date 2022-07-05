/*
 * show the host keys in various formats, for libreswan
 *
 * Copyright (C) 2005 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 1999, 2000, 2001  Henry Spencer.
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2009 Stefan Arentz <stefan@arentz.ca>
 * Copyright (C) 2010, 2016 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012-2013 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2016, 2022 Andrew Cagney <cagney@gnu.org>
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
 * replaces a shell script.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/utsname.h>

#include <arpa/nameser.h>
/* older versions lack ipseckey support */
#ifndef ns_t_ipseckey
# define ns_t_ipseckey 45
#endif

#include "ttodata.h"
#include "constants.h"
#include "lswversion.h"
#include "lswlog.h"
#include "lswalloc.h"
#include "lswconf.h"
#include "secrets.h"
#include "lswnss.h"
#include "lswtool.h"
#include "ip_info.h"

#include <keyhi.h>
#include <prerror.h>
#include <prinit.h>

char usage[] =
	"Usage:\n"
	"   showhostkey --version\n"
	"   showhostkey { --dump | --list }\n"
	"   showhostkey { --left | --right }\n"
	"               { --rsaid <rsaid> | --ckaid <ckaid> }\n"
	"   showhostkey --ipseckey\n"
	"               { --rsaid <rsaid> | --ckaid <ckaid> }\n"
	"               [ --precedence <precedence> ] \n"
	"               [ --gateway <gateway> ]\n"
	"   showhostkey --pem\n"
	"               { --rsaid <rsaid> | --ckaid <ckaid> }\n"
	"Additional options:\n"
	"   --verbose\n"
	"   --debug\n"
	"   --nssdir <nssdir>\n"
	"   --password <password>\n"
	;

/*
 * For new options, avoid magic numbers.
 *
 * XXX: The Can fix old options later.
 */
enum opt {
	OPT_HELP = '?',
	OPT_IPSECKEY = 'K',
	OPT_GATEWAY = 'g',
	OPT_LEFT = 'l',
	OPT_RIGHT = 'r',
	OPT_LIST = 'L',
	OPT_NSSDIR = 'd', /* nss-tools use -d */
	OPT_VERBOSE = 'v',
	OPT_VERSION = 'V',
	OPT_RSAID = 'I',
	OPT_PRECIDENCE = 'p',
	OPT_CONFIGDIR = 256,
	OPT_DUMP,
	OPT_PASSWORD,
	OPT_CKAID,
	OPT_DEBUG,
	OPT_PEM,
};

const char short_opts[] = "v?d:lrg";

struct option long_opts[] = {
	{ "help",       no_argument,            NULL,   OPT_HELP, },
	{ "left",       no_argument,            NULL,   OPT_LEFT, },
	{ "right",      no_argument,            NULL,   OPT_RIGHT, },
	{ "dump",       no_argument,            NULL,   OPT_DUMP, },
	{ "debug",      no_argument,            NULL,   OPT_DEBUG, },
	{ "list",       no_argument,            NULL,   OPT_LIST, },
	{ "ipseckey",   no_argument,            NULL,   OPT_IPSECKEY, },
	{ "gateway",    required_argument,      NULL,   OPT_GATEWAY, },
	{ "precedence", required_argument,      NULL,   OPT_PRECIDENCE, },
	{ "ckaid",      required_argument,      NULL,   OPT_CKAID, },
	{ "rsaid",      required_argument,      NULL,   OPT_RSAID, },
	{ "version",    no_argument,            NULL,   OPT_VERSION, },
	{ "verbose",    no_argument,            NULL,   OPT_VERBOSE, },
	{ "configdir",  required_argument,      NULL,   OPT_CONFIGDIR, }, /* obsoleted */
	{ "nssdir",     required_argument,      NULL,   OPT_NSSDIR, }, /* nss-tools use -d */
	{ "password",   required_argument,      NULL,   OPT_PASSWORD, },
	{ "pem",        no_argument,            NULL,   OPT_PEM, },
	{ 0,            0,                      NULL,   0, }
};

static void print_secret(const char *kind, const char *name,
			 const char *idb, chunk_t secret, bool disclose)
{
	printf("%s keyid: %s\n", kind, idb);
	if (disclose) {
		char pskbuf[128] = "";
		datatot(secret.ptr, secret.len, 'x', pskbuf, sizeof(pskbuf));
		printf("    %s: \"%s\"\n", name, pskbuf);
	}
}

static void print(struct private_key_stuff *pks,
		  int count, struct id *id, bool disclose)
{
	id_buf idbuf = { "n/a", };
	if (id != NULL) {
		str_id(id, &idbuf);
	}
	const char *idb = idbuf.buf;

	if (count) {
		/* ipsec.secrets format */
		printf("%d(%d): ", pks->line, count);
	} else {
		/* NSS format */
		printf("<%2d> ", pks->line);
	}

	switch (pks->kind) {
	case PKK_PSK:
		print_secret("PSK", "psk", idb, pks->u.preshared_secret, disclose);
		break;

	case PKK_XAUTH:
		print_secret("XAUTH", "xauth", idb, pks->u.preshared_secret, disclose);
		break;

	case PKK_RSA:
	case PKK_ECDSA:
	{
		printf("%s", pks->pubkey_type->name);
		keyid_t keyid = pks->keyid;
		printf(" keyid: %s", str_keyid(keyid)[0] ? str_keyid(keyid) : "<missing-pubkey>");
		if (id) {
			printf(" id: %s", idb);
		}
		ckaid_buf cb;
		ckaid_t *ckaid = &pks->ckaid;
		printf(" ckaid: %s\n", str_ckaid(ckaid, &cb));
		break;
	}

	case PKK_PPK:
		break;

	case PKK_NULL:
		/* can't happen but the compiler does not know that */
		printf("NULL authentication -- cannot happen: %s\n", idb);
		abort();

	case PKK_INVALID:
		printf("Invalid or unknown key: %s\n", idb);
		exit(1);
	}
}

static void print_key(struct secret *secret,
		      struct private_key_stuff *pks,
		      bool disclose)
{
	if (secret) {
		int count = 1;
		struct id_list *l = lsw_get_idlist(secret);
		while (l != NULL) {
			print(pks, count, &l->id, disclose);
			l = l->next;
			count++;
		}
	} else {
		print(pks, 0, NULL, disclose);
	}
}

static int list_key(struct secret *secret,
		    struct private_key_stuff *pks,
		    void *uservoid UNUSED)
{
	print_key(secret, pks, false);
	return 1;
}

static int dump_key(struct secret *secret,
		    struct private_key_stuff *pks,
		    void *uservoid UNUSED)
{
	print_key(secret, pks, true);
	return 1;
}

static int pick_by_rsaid(struct secret *secret UNUSED,
			 struct private_key_stuff *pks,
			 void *uservoid)
{
	char *rsaid = (char *)uservoid;

	if (pks->kind == PKK_RSA && streq(pks->keyid.keyid, rsaid)) {
		/* stop */
		return 0;
	} else {
		/* try again */
		return 1;
	}
}

static int pick_by_ckaid(struct secret *secret UNUSED,
			 struct private_key_stuff *pks,
			 void *uservoid)
{
	char *start = (char *)uservoid;
	if ((pks->kind == PKK_RSA || pks->kind == PKK_ECDSA) &&
	    ckaid_starts_with(&pks->ckaid, start)) {
		/* stop */
		return 0;
	} else {
		/* try again */
		return 1;
	}
}

static char *base64_from_chunk(chunk_t chunk)
{
	/*
	 * A byte is 8-bits, base64 uses 6-bits (2^6=64).  Plus some
	 * for 0s.  Plus some for \0.  Plus some extra for rounding.
	 */
	size_t len = chunk.len * 8 / 6 + 2 + 1 + 10;
	char *base64 = alloc_bytes(len, "base64");
	size_t n = datatot(chunk.ptr, chunk.len, 64, base64, len);
	passert(n < len);
	return base64;
}

static char *base64_dnssec_pubkey_from_pks(struct private_key_stuff *pks)
{
	chunk_t chunk = empty_chunk; /* must free */
	err_t e = pks->pubkey_type->pubkey_content_to_dnssec_pubkey(&pks->u.pubkey, &chunk);
	if (e != NULL) {
		fprintf(stderr, "%s: %s\n", progname, e);
		return NULL;
	}

	char *base64 = base64_from_chunk(chunk);
	free_chunk_content(&chunk);
	return base64;
}

static int show_dnskey(struct private_key_stuff *pks,
		       int precedence,
		       char *gateway)
{
	char qname[256];
	int gateway_type = 0;

	gethostname(qname, sizeof(qname));

	char *base64 = base64_dnssec_pubkey_from_pks(pks);
	if (base64 == NULL) {
		return 5;
	}

	if (gateway != NULL) {
		/* XXX: ttoaddress_dns() - knows how to figure out IPvX? */
		ip_address test;
		if (ttoaddress_dns(shunk1(gateway), &ipv4_info, &test) == NULL) {
			gateway_type = 1;
		} else if (ttoaddress_dns(shunk1(gateway), &ipv6_info, &test) == NULL) {
			gateway_type = 2;
		} else {
			fprintf(stderr, "%s: unknown address family for gateway %s",
				progname, gateway);
			return 5;
		}
	}

	printf("%s.    IN    IPSECKEY  %d %d %d %s %s\n",
	       qname, precedence, gateway_type,
	       pks->pubkey_type->alg,
	       (gateway == NULL) ? "." : gateway, base64);
	pfree(base64);
	return 0;
}

static int show_pem(struct private_key_stuff *pks)
{
	chunk_t der = empty_chunk; /* must free */
	err_t e = pks->pubkey_type->pubkey_content_to_der(&pks->u.pubkey, &der);
	if (e != NULL) {
		fprintf(stderr, "%s: %s\n", progname, e);
		return 5;
	}

	char *pem = base64_from_chunk(der);
	free_chunk_content(&der);

	printf("-----BEGIN PUBLIC KEY-----\n");
	printf("%s\n", pem);
	printf("-----END PUBLIC KEY-----\n");

	/*
	 * The output should be accepted by
	 * openssl pkey -in /tmp/x -inform PEM -pubin -noout -text
	 */

	pfreeany(pem);

	return 0;
}

static int show_confkey(struct private_key_stuff *pks,
			char *side)
{
	if (pks->kind != PKK_RSA && pks->kind != PKK_ECDSA) {
		char *enumstr = "gcc is crazy";
		switch (pks->kind) {
		case PKK_PSK:
			enumstr = "PKK_PSK";
			break;
		case PKK_XAUTH:
			enumstr = "PKK_XAUTH";
			break;
		default:
			sscanf(enumstr, "UNKNOWN (%d)", (int *)pks->kind);
		}
		fprintf(stderr, "%s: wrong kind of key %s in show_confkey. Expected PKK_RSA.\n",
			progname, enumstr);
		return 5;
	}

	char *base64 = base64_dnssec_pubkey_from_pks(pks);
	if (base64 == NULL) {
		return 5;
	}

	switch (pks->kind) {
	case PKK_RSA:
		printf("\t# rsakey %s\n",
		       pks->keyid.keyid);
		printf("\t%srsasigkey=0s%s\n", side,
		       base64);
		pfree(base64);
		break;
	case PKK_ECDSA:
		printf("\t# ecdsakey %s\n",
		       pks->keyid.keyid);
		printf("\t%secdsakey=0s%s\n", side,
		       base64);
		pfree(base64);
		break;
	default:
		passert(false);
	}

	return 0;
}

static struct private_key_stuff *lsw_nss_foreach_private_key_stuff(secret_eval func,
								   void *uservoid,
								   struct logger *logger)
{
	PK11SlotInfo *slot = lsw_nss_get_authenticated_slot(logger);
	if (slot == NULL) {
		/* already logged */
		return NULL;
	}

	SECKEYPrivateKeyList *list = PK11_ListPrivateKeysInSlot(slot);
	if (list == NULL) {
		llog(RC_LOG_SERIOUS|ERROR_STREAM, logger, "no list");
		PK11_FreeSlot(slot);
		return NULL;
	}

	int line = 1;

	struct private_key_stuff *result = NULL;

	SECKEYPrivateKeyListNode *node;
	for (node = PRIVKEY_LIST_HEAD(list);
	     !PRIVKEY_LIST_END(node, list);
	     node = PRIVKEY_LIST_NEXT(node)) {

		SECKEYPrivateKey *private_key = node->key;

		/*
		 * XXX: this code has a lot in common with secrets.c
		 * which also creates private-key-stuff.
		 */

		/* XXX: see also private_key_type_nss(pubk); */
		const struct pubkey_type *type;
		switch (SECKEY_GetPrivateKeyType(private_key)) {
		case rsaKey:
			type = &pubkey_type_rsa;
			break;
		case ecKey:
			type = &pubkey_type_ecdsa;
			break;
		default:
			continue;
		}

		SECItem *ckaid_nss = PK11_GetLowLevelKeyIDForPrivateKey(node->key); /* must free */
		if (ckaid_nss == NULL) {
			continue;
		}

		SECKEYPublicKey *pubk = SECKEY_ConvertToPublicKey(node->key);
		if (pubk == NULL) {
			continue;
		}

		struct private_key_stuff pks = {
			.pubkey_type = type,
			.kind = type->private_key_kind,
			.line = 0,
			.private_key = SECKEY_CopyPrivateKey(private_key), /* add reference */
		};

		type->extract_private_key_pubkey_content(&pks, &pks.keyid, &pks.ckaid, &pks.size,
							 pubk, ckaid_nss);

		/*
		 * Only count private keys that get processed.
		 */
		pks.line = line++;

		int ret = func(NULL, &pks, uservoid);
		if (ret == 0) {
			/*
			 * save/return the result.
			 *
			 * XXX: Potential Memory leak.
			 *
			 * lsw_foreach_secret() + lsw_get_pks()
			 * returns an object that must not be freed
			 * BUT lsw_nss_foreach_private_key_stuff()
			 * returns an object that must be freed.
			 *
			 * For moment ignore this - as only caller is
			 * showhostkey.c which quickly exits.
			 */
			result = clone_thing(pks, "pks");
			break;
		}

		type->free_secret_content(&pks);

		if (ret < 0) {
			break;
		}
	}

	SECKEY_DestroyPrivateKeyList(list);
	PK11_FreeSlot(slot);

	return result; /* could be NULL */
}

static struct private_key_stuff *foreach_secret(secret_eval func, void *uservoid, struct logger *logger)
{
	struct private_key_stuff *pks = lsw_nss_foreach_private_key_stuff(func, uservoid, logger);
	if (pks == NULL) {
		/* already logged any error */
		return NULL;
	}
	return pks;
}

int main(int argc, char *argv[])
{
	log_to_stderr = false;
	struct logger *logger = tool_init_log("ipsec showhostkey");

	int opt;
	bool left_flg = false;
	bool right_flg = false;
	bool dump_flg = false;
	bool list_flg = false;
	bool ipseckey_flg = false;
	bool pem_flg = false;
	char *gateway = NULL;
	int precedence = 10;
	char *ckaid = NULL;
	char *rsaid = NULL;

	while ((opt = getopt_long(argc, argv, short_opts, long_opts, NULL)) != EOF) {
		switch (opt) {
		case OPT_HELP:
			goto usage;
			break;

		case OPT_LEFT:
			left_flg = true;
			break;
		case OPT_RIGHT:
			right_flg = true;
			break;

		case OPT_DUMP:
			dump_flg = true;
			break;

		case OPT_IPSECKEY:
			ipseckey_flg = true;
			break;

		case OPT_PEM:
			pem_flg = true;
			break;

		case OPT_PRECIDENCE:
			{
				unsigned long u;
				err_t ugh = ttoulb(optarg, 0, 10, 255, &u);

				if (ugh != NULL) {
					fprintf(stderr,
						"%s: precedence malformed: %s\n", progname, ugh);
					exit(5);
				}
				precedence = u;
			}
			break;
		case OPT_LIST:
			list_flg = true;
			break;

		case OPT_GATEWAY:
			ipseckey_flg = true;
			gateway = clone_str(optarg, "gateway");
			break;

		case OPT_CKAID:
			ckaid = clone_str(optarg, "ckaid");
			break;

		case OPT_RSAID:
			rsaid = clone_str(optarg, "rsaid");
			break;

		case OPT_CONFIGDIR:	/* Obsoletd by --nssdir|-d */
		case OPT_NSSDIR:
			lsw_conf_nssdir(optarg, logger);
			break;

		case OPT_PASSWORD:
			lsw_conf_nsspassword(optarg);
			break;

		case OPT_VERBOSE:
			log_to_stderr = true;
			break;

		case OPT_DEBUG:
			cur_debugging = -1;
			break;

		case OPT_VERSION:
			fprintf(stdout, "%s\n", ipsec_version_string());
			exit(0);

		default:
			goto usage;
		}
	}

	if (!(left_flg + right_flg + ipseckey_flg + pem_flg + dump_flg + list_flg)) {
		fprintf(stderr, "%s: You must specify an operation\n", progname);
		goto usage;
	}

	if ((left_flg + right_flg + ipseckey_flg + pem_flg + dump_flg + list_flg) > 1) {
		fprintf(stderr, "%s: You must specify only one operation\n",
			progname);
		goto usage;
	}

	if ((left_flg + right_flg + ipseckey_flg + pem_flg) && !ckaid && !rsaid) {
		fprintf(stderr, "%s: You must select a key using --ckaid or --rsaid\n",
			progname);
		goto usage;
	}

	if ((dump_flg + list_flg) && (ckaid || rsaid)) {
		fprintf(stderr, "%s: You must not select a key\n",
			progname);
		goto usage;
	}

	/*
	 * Don't fetch the config options until after they have been
	 * processed, and really are "constant".
	 */
	const struct lsw_conf_options *oco = lsw_init_options();
	llog(RC_LOG, logger, "using nss directory \"%s\"", oco->nssdir);

	/*
	 * Set up for NSS - contains key pairs.
	 */
	diag_t d = lsw_nss_setup(oco->nssdir, LSW_NSS_READONLY, logger);
	if (d != NULL) {
		fatal_diag(1, logger, &d, "%s", "");
	}

	int status = 0;

	/* options that apply to entire files */
	if (dump_flg) {
		/* dumps private key info too */
		foreach_secret(dump_key, NULL, logger);
		goto out;
	}

	if (list_flg) {
		foreach_secret(list_key, NULL, logger);
		goto out;
	}

	struct private_key_stuff *pks;
	if (rsaid != NULL) {
		if (log_to_stderr)
			printf("%s picking by rsaid=%s\n",
			       ipseckey_flg ? ";" : "\t#", rsaid);
		pks = foreach_secret(pick_by_rsaid, rsaid, logger);
	} else if (ckaid != NULL) {
		if (log_to_stderr) {
			printf("%s picking by ckaid=%s\n",
			       ipseckey_flg ? ";" : "\t#", ckaid);
		}
		pks = foreach_secret(pick_by_ckaid, ckaid, logger);
	} else {
		fprintf(stderr, "%s: nothing to do\n", progname);
		status = 1;
		goto out;
	}

	if (pks == NULL) {
		fprintf(stderr, "%s: No keys found\n", progname);
		status = 20;
		goto out;
	}

	if (left_flg) {
		status = show_confkey(pks, "left");
		goto out;
	}

	if (right_flg) {
		status = show_confkey(pks, "right");
		goto out;
	}

	if (ipseckey_flg) {
		status = show_dnskey(pks, precedence, gateway);
		goto out;
	}

	if (pem_flg) {
		status = show_pem(pks);
		goto out;
	}

out:
	/*
	 * XXX: pks is being leaked.
	 *
	 * Problem is that for a secret the PKS can't be freed but for
	 * NSS it can.  Not really a problem since the entire secret
	 * table gets leaked anyway.
	 */
	lsw_nss_shutdown();
	exit(status);

usage:
	fputs(usage, stderr);
	exit(1);
}
