/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file onion_tap.c
 * \brief Functions to implement the original Tor circuit extension handshake
 * (a.k.a TAP).
 *
 * We didn't call it "TAP" ourselves -- Ian Goldberg named it in "On the
 * Security of the Tor Authentication Protocol".  (Spoiler: it's secure, but
 * its security is kind of fragile and implementation dependent.  Never modify
 * this implementation without reading and understanding that paper at least.)
 **/

#include "or.h"
#include "config.h"
#include "onion_tap.h"
#include "rephist.h"

/*----------------------------------------------------------------------*/

/** Given a router's 128 byte public key,
 * stores the following in onion_skin_out:
 *   - [42 bytes] OAEP padding
 *   - [16 bytes] Symmetric key for encrypting blob past RSA
 *   - [70 bytes] g^x part 1 (inside the RSA)
 *   - [58 bytes] g^x part 2 (symmetrically encrypted)
 *
 * Stores the DH private key into handshake_state_out for later completion
 * of the handshake.
 *
 * The meeting point/cookies and auth are zeroed out for now.
 */
int
onion_skin_create(crypto_pk_t *dest_router_key,
                  crypto_dh_t **handshake_state_out,
                  char *onion_skin_out) /* ONIONSKIN_CHALLENGE_LEN bytes */
{
  char challenge[DH_KEY_LEN];
  crypto_dh_t *dh = NULL;
  int dhbytes, pkbytes;

  tor_assert(dest_router_key);
  tor_assert(handshake_state_out);
  tor_assert(onion_skin_out);
  *handshake_state_out = NULL;
  memset(onion_skin_out, 0, ONIONSKIN_CHALLENGE_LEN);

  if (!(dh = crypto_dh_new(DH_TYPE_CIRCUIT)))
    goto err;

  dhbytes = crypto_dh_get_bytes(dh);
  pkbytes = (int) crypto_pk_keysize(dest_router_key);
  tor_assert(dhbytes == 128);
  tor_assert(pkbytes == 128);

  if (crypto_dh_get_public(dh, challenge, dhbytes))
    goto err;

  note_crypto_pk_op(ENC_ONIONSKIN);

  /* set meeting point, meeting cookie, etc here. Leave zero for now. */
  if (crypto_pk_public_hybrid_encrypt(dest_router_key, onion_skin_out,
                                      ONIONSKIN_CHALLENGE_LEN,
                                      challenge, DH_KEY_LEN,
                                      PK_PKCS1_OAEP_PADDING, 1)<0)
    goto err;

  memwipe(challenge, 0, sizeof(challenge));
  *handshake_state_out = dh;

  return 0;
 err:
  memwipe(challenge, 0, sizeof(challenge));
  if (dh) crypto_dh_free(dh);
  return -1;
}

/** Given an encrypted DH public key as generated by onion_skin_create,
 * and the private key for this onion router, generate the reply (128-byte
 * DH plus the first 20 bytes of shared key material), and store the
 * next key_out_len bytes of key material in key_out.
 */
int
onion_skin_server_handshake(const char *onion_skin, /*ONIONSKIN_CHALLENGE_LEN*/
                            crypto_pk_t *private_key,
                            crypto_pk_t *prev_private_key,
                            char *handshake_reply_out, /*ONIONSKIN_REPLY_LEN*/
                            char *key_out,
                            size_t key_out_len)
{
  char challenge[ONIONSKIN_CHALLENGE_LEN];
  crypto_dh_t *dh = NULL;
  ssize_t len;
  char *key_material=NULL;
  size_t key_material_len=0;
  int i;
  crypto_pk_t *k;

  len = -1;
  for (i=0;i<2;++i) {
    k = i==0?private_key:prev_private_key;
    if (!k)
      break;
    note_crypto_pk_op(DEC_ONIONSKIN);
    len = crypto_pk_private_hybrid_decrypt(k, challenge,
                                           ONIONSKIN_CHALLENGE_LEN,
                                           onion_skin, ONIONSKIN_CHALLENGE_LEN,
                                           PK_PKCS1_OAEP_PADDING,0);
    if (len>0)
      break;
  }
  if (len<0) {
    log_info(LD_PROTOCOL,
             "Couldn't decrypt onionskin: client may be using old onion key");
    goto err;
  } else if (len != DH_KEY_LEN) {
    log_warn(LD_PROTOCOL, "Unexpected onionskin length after decryption: %ld",
             (long)len);
    goto err;
  }

  dh = crypto_dh_new(DH_TYPE_CIRCUIT);
  if (!dh) {
    log_warn(LD_BUG, "Couldn't allocate DH key");
    goto err;
  }
  if (crypto_dh_get_public(dh, handshake_reply_out, DH_KEY_LEN)) {
    log_info(LD_GENERAL, "crypto_dh_get_public failed.");
    goto err;
  }

  key_material_len = DIGEST_LEN+key_out_len;
  key_material = tor_malloc(key_material_len);
  len = crypto_dh_compute_secret(LOG_PROTOCOL_WARN, dh, challenge,
                                 DH_KEY_LEN, key_material,
                                 key_material_len);
  if (len < 0) {
    log_info(LD_GENERAL, "crypto_dh_compute_secret failed.");
    goto err;
  }

  /* send back H(K|0) as proof that we learned K. */
  memcpy(handshake_reply_out+DH_KEY_LEN, key_material, DIGEST_LEN);

  /* use the rest of the key material for our shared keys, digests, etc */
  memcpy(key_out, key_material+DIGEST_LEN, key_out_len);

  memwipe(challenge, 0, sizeof(challenge));
  memwipe(key_material, 0, key_material_len);
  tor_free(key_material);
  crypto_dh_free(dh);
  return 0;
 err:
  memwipe(challenge, 0, sizeof(challenge));
  if (key_material) {
    memwipe(key_material, 0, key_material_len);
    tor_free(key_material);
  }
  if (dh) crypto_dh_free(dh);

  return -1;
}

/** Finish the client side of the DH handshake.
 * Given the 128 byte DH reply + 20 byte hash as generated by
 * onion_skin_server_handshake and the handshake state generated by
 * onion_skin_create, verify H(K) with the first 20 bytes of shared
 * key material, then generate key_out_len more bytes of shared key
 * material and store them in key_out.
 *
 * After the invocation, call crypto_dh_free on handshake_state.
 */
int
onion_skin_client_handshake(crypto_dh_t *handshake_state,
            const char *handshake_reply, /* ONIONSKIN_REPLY_LEN bytes */
            char *key_out,
            size_t key_out_len)
{
  ssize_t len;
  char *key_material=NULL;
  size_t key_material_len;
  tor_assert(crypto_dh_get_bytes(handshake_state) == DH_KEY_LEN);

  key_material_len = DIGEST_LEN + key_out_len;
  key_material = tor_malloc(key_material_len);
  len = crypto_dh_compute_secret(LOG_PROTOCOL_WARN, handshake_state,
                                 handshake_reply, DH_KEY_LEN, key_material,
                                 key_material_len);
  if (len < 0)
    goto err;

  if (tor_memneq(key_material, handshake_reply+DH_KEY_LEN, DIGEST_LEN)) {
    /* H(K) does *not* match. Something fishy. */
    log_warn(LD_PROTOCOL,"Digest DOES NOT MATCH on onion handshake. "
             "Bug or attack.");
    goto err;
  }

  /* use the rest of the key material for our shared keys, digests, etc */
  memcpy(key_out, key_material+DIGEST_LEN, key_out_len);

  memwipe(key_material, 0, key_material_len);
  tor_free(key_material);
  return 0;
 err:
  memwipe(key_material, 0, key_material_len);
  tor_free(key_material);
  return -1;
}

