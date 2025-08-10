#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "nostr/nip49/nip49.h"

static int parse_hex32(const char *hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return -1;
  for (int i=0;i<32;i++) {
    unsigned int b;
    if (sscanf(hex + i*2, "%02x", &b) != 1) return -1;
    out[i] = (uint8_t)b;
  }
  return 0;
}

static void print_hex32(const uint8_t in[32]) {
  for (int i=0;i<32;i++) printf("%02x", in[i]);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s encrypt|decrypt ...\n", argv[0]);
    return 2;
  }
  if (strcmp(argv[1], "encrypt") == 0) {
    const char *hex = NULL; const char *pw = NULL; int logn = 18; int sec = 1;
    for (int i=2;i<argc;i++) {
      if (strcmp(argv[i], "--privkey-hex")==0 && i+1<argc) hex=argv[++i];
      else if (strcmp(argv[i], "--password")==0 && i+1<argc) pw=argv[++i];
      else if (strcmp(argv[i], "--log-n")==0 && i+1<argc) logn=atoi(argv[++i]);
      else if (strcmp(argv[i], "--security")==0 && i+1<argc) sec=atoi(argv[++i]);
    }
    if (!hex || !pw) { fprintf(stderr, "missing --privkey-hex/--password\n"); return 2; }
    uint8_t sk[32]; if (parse_hex32(hex, sk)!=0) { fprintf(stderr, "bad hex\n"); return 2; }
    char *out = NULL;
    int rc = nostr_nip49_encrypt(sk, (NostrNip49SecurityByte)sec, pw, (uint8_t)logn, &out);
    if (rc != 0) { fprintf(stderr, "encrypt failed (%d)\n", rc); return 1; }
    printf("%s\n", out);
    free(out);
    return 0;
  } else if (strcmp(argv[1], "decrypt") == 0) {
    const char *enc = NULL; const char *pw = NULL;
    for (int i=2;i<argc;i++) {
      if (strcmp(argv[i], "--ncryptsec")==0 && i+1<argc) enc=argv[++i];
      else if (strcmp(argv[i], "--password")==0 && i+1<argc) pw=argv[++i];
    }
    if (!enc || !pw) { fprintf(stderr, "missing --ncryptsec/--password\n"); return 2; }
    uint8_t sk[32]; uint8_t ln=0; NostrNip49SecurityByte sec=0;
    int rc = nostr_nip49_decrypt(enc, pw, sk, &sec, &ln);
    if (rc != 0) { fprintf(stderr, "decrypt failed (%d)\n", rc); return 1; }
    print_hex32(sk); printf("\nlog_n=%u security=%u\n", (unsigned)ln, (unsigned)sec);
    return 0;
  }
  fprintf(stderr, "unknown subcommand\n");
  return 2;
}
