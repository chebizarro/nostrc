#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include "nostr_secrets.h"

int main(void){
  GError *err=NULL;
  GPid pid=0;
  gchar *argv[] = { "./mock_signer", NULL };
  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &pid, &err)){
    fprintf(stderr, "failed to spawn mock_signer: %s\n", err?err->message:"error");
    if (err) g_error_free(err);
    return 77; /* skip */
  }
  /* Give signer a moment */
  g_usleep(200*1000);

  char *pt=NULL; int rc = nh_secrets_decrypt_via_signer("ciphertext-demo", &pt);
  if (rc != 0 || !pt){ fprintf(stderr, "decrypt failed\n"); return 1; }
  if (strcmp(pt, "decrypted:ciphertext-demo") != 0){ fprintf(stderr, "unexpected plaintext: %s\n", pt); free(pt); return 1; }
  free(pt);

  /* Clean up signer */
  g_spawn_close_pid(pid);
  return 0;
}
