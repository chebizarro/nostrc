#ifndef RELAYD_CONN_H
#define RELAYD_CONN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Per-connection state for the 'nostr' protocol */
typedef struct {
  void *it;                /* storage iterator */
  char subid[128];         /* active subscription id */
  int authed;              /* NIP-42 auth state */
  int need_auth_chal;      /* send AUTH challenge on next writeable */
  char auth_chal[64];      /* simple challenge string */
  char authed_pubkey[128]; /* hex pubkey of authenticated client */
  /* Rate limiting */
  unsigned int rl_tokens;
  unsigned long long rl_last_ms;
  unsigned int rl_ops_per_sec;
  unsigned int rl_burst;
  /* NIP-77 Negentropy state */
  void *neg_state;         /* storage negentropy session state */
  char neg_subid[128];     /* negentropy subscription id */
} ConnState;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CONN_H */
