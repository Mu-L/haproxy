/*
 * JSON Web Token (JWT) processing
 *
 * Copyright 2021 HAProxy Technologies
 * Remi Tricot-Le Breton <rlebreton@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <sys/stat.h>

#include <import/ebmbtree.h>
#include <import/ebsttree.h>

#include <haproxy/api.h>
#include <haproxy/tools.h>
#include <haproxy/openssl-compat.h>
#include <haproxy/base64.h>
#include <haproxy/jwt.h>
#include <haproxy/buf.h>
#include <haproxy/ssl_ckch.h>
#include <haproxy/ssl_sock.h>


#ifdef USE_OPENSSL

/* Tree into which the public certificates used to validate JWTs will be stored. */
struct eb_root jwt_cert_tree = EB_ROOT_UNIQUE;
__decl_rwlock(jwt_tree_lock);


/*
 * The possible algorithm strings that can be found in a JWS's JOSE header are
 * defined in section 3.1 of RFC7518.
 */
enum jwt_alg jwt_parse_alg(const char *alg_str, unsigned int alg_len)
{
	enum jwt_alg alg = JWT_ALG_DEFAULT;

	/* Algorithms are all 5 characters long apart from "none". */
	if (alg_len < sizeof("HS256")-1) {
		if (alg_len == sizeof("none")-1 && strcmp("none", alg_str) == 0)
			alg = JWS_ALG_NONE;
		return alg;
	}

	if (alg == JWT_ALG_DEFAULT) {
		switch(*alg_str++) {
		case 'H':
			if (strncmp(alg_str, "S256", alg_len-1) == 0)
				alg = JWS_ALG_HS256;
			else if (strncmp(alg_str, "S384", alg_len-1) == 0)
				alg = JWS_ALG_HS384;
			else if (strncmp(alg_str, "S512", alg_len-1) == 0)
				alg = JWS_ALG_HS512;
			break;
		case 'R':
			if (strncmp(alg_str, "S256", alg_len-1) == 0)
				alg = JWS_ALG_RS256;
			else if (strncmp(alg_str, "S384", alg_len-1) == 0)
				alg = JWS_ALG_RS384;
			else if (strncmp(alg_str, "S512", alg_len-1) == 0)
				alg = JWS_ALG_RS512;
			break;
		case 'E':
			if (strncmp(alg_str, "S256", alg_len-1) == 0)
				alg = JWS_ALG_ES256;
			else if (strncmp(alg_str, "S384", alg_len-1) == 0)
				alg = JWS_ALG_ES384;
			else if (strncmp(alg_str, "S512", alg_len-1) == 0)
				alg = JWS_ALG_ES512;
			break;
		case 'P':
			if (strncmp(alg_str, "S256", alg_len-1) == 0)
				alg = JWS_ALG_PS256;
			else if (strncmp(alg_str, "S384", alg_len-1) == 0)
				alg = JWS_ALG_PS384;
			else if (strncmp(alg_str, "S512", alg_len-1) == 0)
				alg = JWS_ALG_PS512;
			break;
		default:
			break;
		}
	}

	return alg;
}

/*
 * Split a JWT into its separate dot-separated parts.
 * Since only JWS following the Compact Serialization format are managed for
 * now, we don't need to manage more than three subparts in the tokens.
 * See section 3.1 of RFC7515 for more information about JWS Compact
 * Serialization.
 * Returns 0 in case of success.
 */
int jwt_tokenize(const struct buffer *jwt, struct jwt_item *items, unsigned int *item_num)
{
	char *ptr = jwt->area;
	char *jwt_end = jwt->area + jwt->data;
	unsigned int index = 0;
	unsigned int length = 0;

	if (index < *item_num) {
		items[index].start = ptr;
		items[index].length = 0;
	}

	while (index < *item_num && ptr < jwt_end) {
		if (*ptr++ == '.') {
			items[index++].length = length;

			if (index == *item_num)
				return -1;
			items[index].start = ptr;
			items[index].length = 0;
			length = 0;
		} else
			++length;
	}

	if (index < *item_num)
		items[index].length = length;

	*item_num = (index+1);

	return (ptr != jwt_end);
}

/*
 * Parse a public certificate and insert it into the jwt_cert_tree.
 * This function can only be called during configuration parsing so we do not
 * need to lock the jwt certificate tree.
 * Returns 0 in case of success.
 */
int jwt_tree_load_cert(char *path, int pathlen, const char *file, int line, char **err)
{
	int retval = -1;
	struct jwt_cert_tree_entry *entry = NULL;
	EVP_PKEY *pubkey = NULL;
	BIO *bio = NULL;
	struct stat buf;
	struct ebmb_node *eb = NULL;
	struct ckch_store *store = NULL;

	eb = ebst_lookup(&jwt_cert_tree, path);

	if (eb)
		return 0; /* Entry already in the tree, nothing to do. */

	entry = calloc(1, sizeof(*entry) + pathlen + 1);
	if (!entry) {
		memprintf(err, "%sunable to allocate memory (jwt_cert_tree_entry).\n", err && *err ? *err : "");
		return -1;
	}
	memcpy(entry->path, path, pathlen + 1);
	entry->type = JWT_ENTRY_DFLT;

	if (ebst_insert(&jwt_cert_tree, &entry->node) != &entry->node) {
		/* Should never happen since we checked if the entry already
		 * existed previously.
		 */
		free(entry);
		return 0;
	}

	if (stat(path, &buf) == 0) {
		bio = BIO_new(BIO_s_file());
		if (!bio) {
			memprintf(err, "%sunable to allocate memory (BIO).\n", err && *err ? *err : "");
			goto end;
		}

		if (BIO_read_filename(bio, path) == 1) {
			pubkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);

			/* The file might exist but not contain a public key if
			 * we were given an actual certificate path (or a
			 * named crt-store).
			 */
			if (pubkey) {
				entry->type = JWT_ENTRY_PKEY;
				entry->pubkey = pubkey;
				retval = 0;
				goto end;
			}
		}
	}

	/* Look for an actual certificate or crt-store with the given name.
	 * If the path corresponds to an actual certificate that was not loaded
	 * yet we will create the corresponding ckch_store. */
	if (HA_SPIN_TRYLOCK(CKCH_LOCK, &ckch_lock))
		goto end;

	store = ckchs_lookup(path);
	if (!store) {
		struct ckch_conf conf = {};
		int err_code = 0;

		/* Create a new store with the given path */
		store = ckch_store_new(path);
		if (!store) {
			HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);
			goto end;
		}

		conf.crt = path;

		err_code = ckch_store_load_files(&conf, store,  0, file, line, err);
		if (err_code & ERR_FATAL) {
			ckch_store_free(store);

			/* If we are in this case we are in the conf
			 * parsing phase and this case might happen if
			 * we were provided an HMAC secret or a variable
			 * name.
			 */
			retval = 0;
			ha_free(err);
			HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);
			goto end;
		}

		if (ebst_insert(&ckchs_tree, &store->node) != &store->node) {
			ckch_store_free(store);
			HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);
			goto end;
		}
	}

	retval = 0;

	BUG_ON(store->jwt_entry != NULL);
	entry->type = JWT_ENTRY_STORE;
	entry->ckch_store = store;
	entry->pubkey = X509_get_pubkey(store->data->cert);
	store->jwt_entry = entry;
	HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);

end:
	if (retval) {
		/* Some error happened during pubkey parsing, remove the already
		 * inserted node from the tree and free it.
		 */
		ebmb_delete(&entry->node);
		free(entry);
	}
	BIO_free(bio);

	return retval;
}


/* Try to look for an already existing ckch_store in the store tree with the
 * path found in the jwt_entry. Keep a reference to its pubkey if it exists.
 * Return 0 in case of success.
 */
static int jwt_tree_tryload_store(struct jwt_cert_tree_entry *jwt_entry)
{
	struct ckch_store *store = NULL;
	int retval = 1;

	if (!jwt_entry)
		return 1;

	/* We might have been given a 'crt-store' name */
	if (HA_SPIN_TRYLOCK(CKCH_LOCK, &ckch_lock))
		return 1;

	store = ckchs_lookup(jwt_entry->path);
	if (!store || store->jwt_entry)
		goto end;

	store->jwt_entry = jwt_entry;

	BUG_ON(jwt_entry->pubkey != NULL);
	jwt_entry->pubkey = X509_get_pubkey(store->data->cert);

	retval = 0;

end:
	HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);
	return retval;

}

/* Update the ckch_store and public key reference of a jwt_entry. This is only
 * useful when updating a certificate from the CLI if it was being used for JWT
 * validation.
 */
void jwt_replace_ckch_store(struct ckch_store *old_ckchs, struct ckch_store *new_ckchs)
{
	struct jwt_cert_tree_entry *entry = old_ckchs->jwt_entry;

	HA_RWLOCK_WRLOCK(JWT_LOCK, &jwt_tree_lock);

	if (entry == NULL)
		goto end;

	old_ckchs->jwt_entry->ckch_store = new_ckchs;
	new_ckchs->jwt_entry = old_ckchs->jwt_entry;

	EVP_PKEY_free(entry->pubkey);
	entry->pubkey = X509_get_pubkey(new_ckchs->data->cert);

end:
	HA_RWLOCK_WRUNLOCK(JWT_LOCK, &jwt_tree_lock);
}

/*
 * Calculate the HMAC signature of a specific JWT and check that it matches the
 * one included in the token.
 * Returns 1 in case of success.
 */
static enum jwt_vrfy_status
jwt_jwsverify_hmac(const struct jwt_ctx *ctx, const struct buffer *decoded_signature)
{
	const EVP_MD *evp = NULL;
	unsigned char signature[EVP_MAX_MD_SIZE];
	unsigned int signature_length = 0;
	unsigned char *hmac_res = NULL;
	enum jwt_vrfy_status retval = JWT_VRFY_KO;

	switch(ctx->alg) {
	case JWS_ALG_HS256:
		evp = EVP_sha256();
		break;
	case JWS_ALG_HS384:
		evp = EVP_sha384();
		break;
	case JWS_ALG_HS512:
		evp = EVP_sha512();
		break;
	default: break;
	}

	hmac_res = HMAC(evp, ctx->key, ctx->key_length, (const unsigned char*)ctx->jose.start,
			ctx->jose.length + ctx->claims.length + 1, signature, &signature_length);

	if (hmac_res && signature_length == decoded_signature->data &&
		  (CRYPTO_memcmp(decoded_signature->area, signature, signature_length) == 0))
		retval = JWT_VRFY_OK;

	return retval;
}

/*
 * Convert a JWT ECDSA signature (R and S parameters concatenatedi, see section
 * 3.4 of RFC7518) into an ECDSA_SIG that can be fed back into OpenSSL's digest
 * verification functions.
 * Returns 0 in case of success.
 */
static int convert_ecdsa_sig(const struct jwt_ctx *ctx, struct buffer *signature)
{
	int retval = 0;
	ECDSA_SIG *ecdsa_sig = NULL;
	BIGNUM *ec_R = NULL, *ec_S = NULL;
	unsigned int bignum_len;
	unsigned char *p;

	ecdsa_sig = ECDSA_SIG_new();
	if (!ecdsa_sig) {
		retval = JWT_VRFY_OUT_OF_MEMORY;
		goto end;
	}

	if (b_data(signature) % 2) {
		retval = JWT_VRFY_INVALID_TOKEN;
		goto end;
	}

	bignum_len = b_data(signature) / 2;

	ec_R = BN_bin2bn((unsigned char*)b_orig(signature), bignum_len, NULL);
	ec_S = BN_bin2bn((unsigned char *)(b_orig(signature) + bignum_len), bignum_len, NULL);

	if (!ec_R || !ec_S) {
		retval = JWT_VRFY_INVALID_TOKEN;
		goto end;
	}

	/* Build ecdsa out of R and S values. */
	ECDSA_SIG_set0(ecdsa_sig, ec_R, ec_S);

	p = (unsigned char*)signature->area;

	signature->data = i2d_ECDSA_SIG(ecdsa_sig, &p);
	if (signature->data == 0) {
		retval = JWT_VRFY_INVALID_TOKEN;
		goto end;
	}

end:
	ECDSA_SIG_free(ecdsa_sig);
	return retval;
}

/*
 * Check that the signature included in a JWT signed via RSA or ECDSA is valid
 * and can be verified thanks to a given public certificate.
 * Returns 1 in case of success.
 */
static enum jwt_vrfy_status
jwt_jwsverify_rsa_ecdsa(const struct jwt_ctx *ctx, struct buffer *decoded_signature)
{
	const EVP_MD *evp = NULL;
	EVP_MD_CTX *evp_md_ctx;
	EVP_PKEY_CTX *pkey_ctx = NULL;
	enum jwt_vrfy_status retval = JWT_VRFY_KO;
	struct ebmb_node *eb = NULL;
	struct jwt_cert_tree_entry *entry = NULL;
	int is_ecdsa = 0;
	int padding = RSA_PKCS1_PADDING;
	EVP_PKEY *pubkey = NULL;
	int lock_iswrlock = 0;

	switch(ctx->alg) {
	case JWS_ALG_RS256:
		evp = EVP_sha256();
		break;
	case JWS_ALG_RS384:
		evp = EVP_sha384();
		break;
	case JWS_ALG_RS512:
		evp = EVP_sha512();
		break;

	case JWS_ALG_ES256:
		evp = EVP_sha256();
		is_ecdsa = 1;
		break;
	case JWS_ALG_ES384:
		evp = EVP_sha384();
		is_ecdsa = 1;
		break;
	case JWS_ALG_ES512:
		evp = EVP_sha512();
		is_ecdsa = 1;
		break;

	case JWS_ALG_PS256:
		evp = EVP_sha256();
		padding = RSA_PKCS1_PSS_PADDING;
		break;
	case JWS_ALG_PS384:
		evp = EVP_sha384();
		padding = RSA_PKCS1_PSS_PADDING;
		break;
	case JWS_ALG_PS512:
		evp = EVP_sha512();
		padding = RSA_PKCS1_PSS_PADDING;
		break;
	default: break;
	}

	evp_md_ctx = EVP_MD_CTX_new();
	if (!evp_md_ctx)
		return JWT_VRFY_OUT_OF_MEMORY;

	HA_RWLOCK_RDLOCK(JWT_LOCK, &jwt_tree_lock);

	eb = ebst_lookup(&jwt_cert_tree, ctx->key);

	if (!eb) {
		HA_RWLOCK_RDUNLOCK(JWT_LOCK, &jwt_tree_lock);

		/* Create new entry and insert it in the jwt cert tree if we
		 * could find a corresponding ckch_store.
		 */
		entry = calloc(1, sizeof(*entry) + ctx->key_length + 1);
		if (!entry) {
			retval = JWT_VRFY_OUT_OF_MEMORY;
			goto end;
		}
		memcpy(entry->path, ctx->key, ctx->key_length);

		/* The ckch_lock will be taken in jwt_tree_tryload_store so we
		 * can't hold the lock on the jwt_cert_tree here because the lock
		 * order is different when updating a certificate from the CLI,
		 * where the ckch_lock is taken first and then the JWT one is
		 * taken in jwt_replace_ckch_store.
		 * If no corresponding ckch_store was found, we still try to
		 * insert the entry in the tree so that next calls to jwt_verify
		 * with the same 'key' path do not perform the lookup in the
		 * ckch_store anymore. */
		entry->type = (jwt_tree_tryload_store(entry) == 0) ? JWT_ENTRY_STORE : JWT_ENTRY_INVALID;

		HA_RWLOCK_WRLOCK(JWT_LOCK, &jwt_tree_lock);
		if (ebst_insert(&jwt_cert_tree, &entry->node) != &entry->node) {
			/* This rather unlikely case can only happen if the tree was
			 * modified between the previous read unlock and here.
			 */
			retval = JWT_VRFY_INTERNAL_ERR;
			free(entry);
			entry = NULL;
			HA_RWLOCK_WRUNLOCK(JWT_LOCK, &jwt_tree_lock);
			goto end;
		}
		lock_iswrlock = 1;
	} else {
		entry = ebmb_entry(eb, struct jwt_cert_tree_entry, node);
	}

	/* We tried looking for a ckch_store but could not find it */
	switch (entry->type) {
	case JWT_ENTRY_PKEY:
	case JWT_ENTRY_STORE:
		pubkey = entry->pubkey;
		if (pubkey)
			EVP_PKEY_up_ref(pubkey);
		break;
	case JWT_ENTRY_DFLT:
	case JWT_ENTRY_INVALID:
		retval = JWT_VRFY_UNKNOWN_CERT;
		break;
	}

	if (lock_iswrlock)
		HA_RWLOCK_WRUNLOCK(JWT_LOCK, &jwt_tree_lock);
	else
		HA_RWLOCK_RDUNLOCK(JWT_LOCK, &jwt_tree_lock);

	if (!pubkey)
		goto end;

	/*
	 * ECXXX signatures are a direct concatenation of the (R, S) pair and
	 * need to be converted back to asn.1 in order for verify operations to
	 * work with OpenSSL.
	 */
	if (is_ecdsa) {
		int conv_retval = convert_ecdsa_sig(ctx, decoded_signature);
		if (conv_retval != 0) {
			retval = conv_retval;
			goto end;
		}
	}

	if (EVP_DigestVerifyInit(evp_md_ctx, &pkey_ctx, evp, NULL, pubkey) == 1) {
		if (is_ecdsa || EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, padding) > 0) {
			if (EVP_DigestVerifyUpdate(evp_md_ctx, (const unsigned char*)ctx->jose.start,
						   ctx->jose.length + ctx->claims.length + 1) == 1 &&
			    EVP_DigestVerifyFinal(evp_md_ctx, (const unsigned char*)decoded_signature->area, decoded_signature->data) == 1) {
				retval = JWT_VRFY_OK;
			}
		}
	}

end:
	EVP_MD_CTX_free(evp_md_ctx);
	if (retval != JWT_VRFY_OK) {
		/* Don't forget to remove SSL errors to be sure they cannot be
		 * caught elsewhere. The error queue is cleared because it seems
		 * at least 2 errors are produced.
		 */
		ERR_clear_error();
	}
	EVP_PKEY_free(pubkey);
	return retval;
}

/*
 * Check that the <token> that was signed via algorithm <alg> using the <key>
 * (either an HMAC secret or the path to a public certificate) has a valid
 * signature.
 * Returns 1 in case of success.
 */
enum jwt_vrfy_status jwt_verify(const struct buffer *token, const struct buffer *alg,
				const struct buffer *key)
{
	struct jwt_item items[JWT_ELT_MAX] = { { 0 } };
	unsigned int item_num = JWT_ELT_MAX;
	struct buffer *decoded_sig = NULL;
	struct jwt_ctx ctx = {};
	enum jwt_vrfy_status retval = JWT_VRFY_KO;
	int ret;

	ctx.alg = jwt_parse_alg(alg->area, alg->data);

	if (ctx.alg == JWT_ALG_DEFAULT)
		return JWT_VRFY_UNKNOWN_ALG;

	if (jwt_tokenize(token, items, &item_num))
		return JWT_VRFY_INVALID_TOKEN;

	if (item_num != JWT_ELT_MAX)
		if (ctx.alg != JWS_ALG_NONE || item_num != JWT_ELT_SIG)
			return JWT_VRFY_INVALID_TOKEN;

	ctx.jose = items[JWT_ELT_JOSE];
	ctx.claims = items[JWT_ELT_CLAIMS];
	ctx.signature = items[JWT_ELT_SIG];

	/* "alg" is "none", the signature must be empty for the JWS to be valid. */
	if (ctx.alg == JWS_ALG_NONE) {
		return (ctx.signature.length == 0) ? JWT_VRFY_OK : JWT_VRFY_KO;
	}

	if (ctx.signature.length == 0)
		return JWT_VRFY_INVALID_TOKEN;

	decoded_sig = alloc_trash_chunk();
	if (!decoded_sig)
		return JWT_VRFY_OUT_OF_MEMORY;

	ret = base64urldec(ctx.signature.start, ctx.signature.length,
	                   decoded_sig->area, decoded_sig->size);
	if (ret == -1) {
		retval = JWT_VRFY_INVALID_TOKEN;
		goto end;
	}

	decoded_sig->data = ret;
	ctx.key = key->area;
	ctx.key_length = key->data;

	/* We have all three sections, signature calculation can begin. */

	switch(ctx.alg) {

	case JWS_ALG_HS256:
	case JWS_ALG_HS384:
	case JWS_ALG_HS512:
		/* HMAC + SHA-XXX */
		retval = jwt_jwsverify_hmac(&ctx, decoded_sig);
		break;
	case JWS_ALG_RS256:
	case JWS_ALG_RS384:
	case JWS_ALG_RS512:
	case JWS_ALG_ES256:
	case JWS_ALG_ES384:
	case JWS_ALG_ES512:
	case JWS_ALG_PS256:
	case JWS_ALG_PS384:
	case JWS_ALG_PS512:
		/* RSASSA-PKCS1-v1_5 + SHA-XXX */
		/* ECDSA using P-XXX and SHA-XXX */
		/* RSASSA-PSS using SHA-XXX and MGF1 with SHA-XXX */
		retval = jwt_jwsverify_rsa_ecdsa(&ctx, decoded_sig);
		break;
	default:
		/* Not managed yet */
		retval = JWT_VRFY_UNMANAGED_ALG;
		break;
	}

end:
	free_trash_chunk(decoded_sig);

	return retval;
}

static void jwt_deinit(void)
{
	struct ebmb_node *node = NULL;
	struct jwt_cert_tree_entry *entry = NULL;

	HA_RWLOCK_WRLOCK(JWT_LOCK, &jwt_tree_lock);

	node = ebmb_first(&jwt_cert_tree);
	while (node) {
		entry = ebmb_entry(node, struct jwt_cert_tree_entry, node);
		ebmb_delete(node);
		EVP_PKEY_free(entry->pubkey);
		ha_free(&entry);
		node = ebmb_first(&jwt_cert_tree);
	}

	HA_RWLOCK_WRUNLOCK(JWT_LOCK, &jwt_tree_lock);
}
REGISTER_POST_DEINIT(jwt_deinit);


#endif /* USE_OPENSSL */
