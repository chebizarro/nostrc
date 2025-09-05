#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <openssl/rand.h>
#include <nostr/nip04.h>
#include <secure_buf.h>

/* hex helpers */
static void bin2hex(const unsigned char *in, size_t len, char *out){
  static const char hexd[] = "0123456789abcdef";
  for (size_t i=0;i<len;i++){ out[2*i] = hexd[(in[i]>>4)&0xF]; out[2*i+1] = hexd[in[i]&0xF]; }
  out[2*len] = '\0';
}

/* Build compressed pubkey (33 bytes) from 32-byte secret */
static int seckey_to_pub_compressed(const unsigned char sk[32], char out_hex[66]){
  int ok = 0;
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  if (!ctx) return 0;
  if (!secp256k1_ec_seckey_verify(ctx, sk)) goto done;
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_create(ctx, &pub, sk)) goto done;
  unsigned char out33[33]; size_t outlen = sizeof(out33);
  if (!secp256k1_ec_pubkey_serialize(ctx, out33, &outlen, &pub, SECP256K1_EC_COMPRESSED)) goto done;
  if (outlen != 33) goto done;
  bin2hex(out33, 33, out_hex);
  ok = 1;
 done:
  secp256k1_context_destroy(ctx);
  return ok;
}

int main(void){
  /* Generate a random seckey */
  unsigned char sk[32];
  if (RAND_bytes(sk, sizeof(sk)) != 1) {
    fprintf(stderr, "RAND failed\n");
    return 1;
  }
  /* Derive peer pub (self) */
  char pub_hex[66+1];
  if (!seckey_to_pub_compressed(sk, pub_hex)){
    fprintf(stderr, "pub derivation failed\n");
    return 1;
  }
  nostr_secure_buf sb = secure_alloc(32);
  memcpy(sb.ptr, sk, 32);
  volatile unsigned char *p = sk; for (size_t i=0;i<sizeof sk;i++) p[i]=0; /* wipe stack copy */

  const char *pt = "hello secure nip04";
  char *out = NULL; char *err = NULL;
  if (nostr_nip04_encrypt_secure(pt, pub_hex, &sb, &out, &err) != 0 || !out){
    fprintf(stderr, "encrypt fail: %s\n", err?err:"(nil)");
    if (err) free(err);
    secure_free(&sb);
    return 2;
  }
  char *dec = NULL; err = NULL;
  if (nostr_nip04_decrypt_secure(out, pub_hex, &sb, &dec, &err) != 0 || !dec){
    fprintf(stderr, "decrypt fail: %s\n", err?err:"(nil)");
    free(out);
    if (err) free(err);
    secure_free(&sb);
    return 3;
  }
  int rc = 0;
  if (strcmp(pt, dec) != 0){
    fprintf(stderr, "mismatch: '%s' vs '%s'\n", pt, dec);
    rc = 4;
  }
  free(out);
  free(dec);
  secure_free(&sb);
  return rc;
}
