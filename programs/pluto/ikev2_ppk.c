/*
 * Helper function for dealing with post-quantum preshared keys
 *
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2019 D. Hugh Redelmeier <hugh@mimosa.com>
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


#include "lswlog.h"

#include "defs.h"

#include "id.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "keys.h" /* needs state.h */
#include "demux.h"
#include "packet.h"
#include "ikev2_prf.h"

#include "ike_alg.h"
#include "crypt_symkey.h"
#include "pluto_crypt.h"
#include "ikev2.h"
#include "ikev2_ppk.h"
#include "ike_alg_hash.h"
#include "crypt_mac.h"
#include "ikev2_auth.h"

/*
 * used by initiator, to properly construct struct
 * from chunk_t we got from .secrets
 */
bool create_ppk_id_payload(chunk_t *ppk_id, struct ppk_id_payload *payl)
{
	payl->type = PPK_ID_FIXED;	/* currently we support only this type */
	payl->ppk_id = *ppk_id;
	return TRUE;
}

/*
 * used by initiator to make chunk_t from ppk_id payload
 * for sending it in PPK_ID Notify Payload over the wire
 */
bool emit_unified_ppk_id(struct ppk_id_payload *payl, pb_stream *pbs)
{
	u_char type = PPK_ID_FIXED;
	return out_raw(&type, sizeof(type), pbs, "PPK_ID_FIXED") &&
		pbs_out_hunk(payl->ppk_id, pbs, "PPK_ID");
}

/*
 * used by responder, for extracting PPK_ID from IKEv2 Notify
 * PPK_ID Payload, we store PPK_ID and its type in payl
 */
bool extract_ppk_id(pb_stream *pbs, struct ppk_id_payload *payl)
{
	size_t len = pbs_left(pbs);
	u_char dst[PPK_ID_MAXLEN];
	int idtype;

	if (len > PPK_ID_MAXLEN) {
		loglog(RC_LOG_SERIOUS, "PPK ID length is too big");
		return FALSE;
	}
	if (len <= 1) {
		loglog(RC_LOG_SERIOUS, "PPK ID data must be at least 1 byte (received %zd bytes including ppk type byte)",
			len);
		return FALSE;
	}

	if (!in_raw(dst, len, pbs, "Unified PPK_ID Payload")) {
		loglog(RC_LOG_SERIOUS, "PPK ID data could not be read");
		return FALSE;
	}

	DBG(DBG_CONTROL, DBG_log("received PPK_ID type: %s",
		enum_name(&ikev2_ppk_id_type_names, dst[0])));

	idtype = (int)dst[0];
	switch (idtype) {
	case PPK_ID_FIXED:
		DBG(DBG_CONTROL, DBG_log("PPK_ID of type PPK_ID_FIXED."));
		break;

	case PPK_ID_OPAQUE:
	default:
		loglog(RC_LOG_SERIOUS, "PPK_ID type %d (%s) not supported",
			idtype, enum_name(&ikev2_ppk_id_type_names, idtype));
		return FALSE;
	}

	/* clone ppk id data without ppk id type byte */
	payl->ppk_id = clone_bytes_as_chunk(dst + 1, len - 1, "PPK_ID data");
	DBG(DBG_CONTROL, DBG_dump_hunk("Extracted PPK_ID", payl->ppk_id));

	return TRUE;
}

stf_status ikev2_calc_no_ppk_auth(struct ike_sa *ike,
				  const struct crypt_mac *id_hash,
				  chunk_t *no_ppk_auth /* output */)
{
	struct connection *c = ike->sa.st_connection;
	enum keyword_authby authby = c->spd.this.authby;

	free_chunk_content(no_ppk_auth);	/* in case it was occupied */

	switch (authby) {
	case AUTHBY_RSASIG:
	{
		const struct hash_desc *hash_algo = v2_auth_negotiated_signature_hash(ike);
		if (hash_algo == NULL) {
			if (c->sighash_policy == LEMPTY) {
				/* RSA with SHA1 without Digsig: no oid blob appended */
				if (!ikev2_calculate_rsa_hash(ike, ike->sa.st_original_role,
							      id_hash, NULL, no_ppk_auth,
							      &ike_alg_hash_sha1)) {
					return STF_FAIL;
				}
				return STF_OK;
			} else {
				loglog(RC_LOG_SERIOUS, "No compatible hash algo");
				return STF_FAIL;
			}
		}

		shunk_t h = hash_algo->hash_asn1_blob_rsa;
		if (h.len == 0) {
			LOG_PEXPECT("negotiated hash algorithm %s has no RSA ASN1 blob",
				    hash_algo->common.fqn);
			return STF_FAIL;
		}

		chunk_t hashval = NULL_HUNK;
		if (!ikev2_calculate_rsa_hash(ike, ike->sa.st_original_role,
					      id_hash, NULL, &hashval,
					      hash_algo)) {
			return STF_FAIL;
		}

		if (ike->sa.st_seen_hashnotify) {
			/*
			 * combine blobs to create no_ppk_auth:
			 * - ASN.1 algo blob
			 * - hashval
			 */
			int len = h.len + hashval.len;
			uint8_t *blobs = alloc_bytes(len,
				"bytes for blobs for AUTH_DIGSIG NO_PPK_AUTH");

			memcpy(&blobs[0], h.ptr, h.len);
			memcpy(&blobs[h.len], hashval.ptr, hashval.len);
			*no_ppk_auth = chunk2(blobs, len);
		}
		free_chunk_content(&hashval);
		return STF_OK;
	}
	case AUTHBY_PSK:
		/* store in no_ppk_auth */
		if (!ikev2_create_psk_auth(AUTHBY_PSK, ike, id_hash, no_ppk_auth)) {
			return STF_INTERNAL_ERROR;
		}
		return STF_OK;

	default:
		bad_case(authby);
	}
}

/* in X_no_ppk keys are stored keys that go into PRF, and we store result in sk_X */

static void ppk_recalc_one(PK11SymKey **sk /* updated */, PK11SymKey *ppk_key, const struct prf_desc *prf_desc, const char *name)
{
	PK11SymKey *t = ikev2_prfplus(prf_desc, ppk_key, *sk, prf_desc->prf_key_size);
	release_symkey(__func__, name, sk);
	*sk = t;
	DBG(DBG_PRIVATE, {
		chunk_t chunk_sk = chunk_from_symkey("sk_chunk", *sk);
		DBG_dump_hunk(name, chunk_sk);
		free_chunk_content(&chunk_sk);
	});
}

void ppk_recalculate(const chunk_t *ppk, const struct prf_desc *prf_desc,
			PK11SymKey **sk_d,	/* updated */
			PK11SymKey **sk_pi,	/* updated */
			PK11SymKey **sk_pr)	/* updated */
{
	PK11SymKey *ppk_key = symkey_from_hunk("PPK Keying material", *ppk);

	DBG(DBG_CRYPT, {
		DBG_log("Starting to recalculate SK_d, SK_pi, SK_pr");
		DBG_dump_hunk("PPK:", *ppk);
	});

	DBGF(DBG_PRIVATE, "PPK recalculating SK_d, SK_pi, SK_pr");

	ppk_recalc_one(sk_d, ppk_key, prf_desc, "sk_d");
	ppk_recalc_one(sk_pi, ppk_key, prf_desc, "sk_pi");
	ppk_recalc_one(sk_pr, ppk_key, prf_desc, "sk_pr");

	release_symkey(__func__, "PPK chunk", &ppk_key);
}
