#define _GNU_SOURCE
#include "fscrypt_lifecycle.h"
#include "fscrypt_kernel.h"
#include "fscrypt_copy.h"
#include <jansson.h>
#include <linux/fs.h>
#include "nh_porthome_at_rest.h"
#include <porthome/crypto.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <poll.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <unistd.h>

/* The authority directory and this database are root-only. In particular none
 * of these records is stored inside a home that removal is about to lock. */
struct nh_fscrypt {
  sqlite3 *db;
  pthread_mutex_t mutex;
  pthread_t thread;
  int event, lock, enable_new, override, started;
  atomic_int stop, watching;
  sd_bus *bus;                /* watcher thread only after startup */
  char boot[40];
};
typedef struct {
  char account[37], home[256], operation[37], state[24];
  uint32_t uid, generation;
  uint64_t dev, ino, epoch;
  uint8_t salt[32], id[16];
  int provisioning;
} record;

static int statement(nh_fscrypt *c, const char *sql, sqlite3_stmt **s) {
  return sqlite3_prepare_v2(c->db, sql, -1, s, NULL) == SQLITE_OK ? 0 : -1;
}
static int exec_sql(nh_fscrypt *c, const char *sql) {
  return sqlite3_exec(c->db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static void wake(nh_fscrypt *c) {
  uint64_t one = 1;
  (void)write(c->event, &one, sizeof one);
}
static int enter(nh_fscrypt *c) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline)) return -1;
  deadline.tv_sec += 5;
  int error = pthread_mutex_timedlock(&c->mutex, &deadline);
  if (error) { errno = error; return -1; }
  if (!flock(c->lock, LOCK_EX | LOCK_NB)) return 0;
  pthread_mutex_unlock(&c->mutex);
  return -1;
}
static void leave(nh_fscrypt *c) {
  (void)flock(c->lock, LOCK_UN);
  pthread_mutex_unlock(&c->mutex);
}
static int load(nh_fscrypt *c, const char *account, record *r) {
  sqlite3_stmt *s = NULL;
  if (statement(c, "SELECT account,home,operation,state,uid,generation,dev,ino,epoch,salt,key_id,provisioning FROM homes WHERE account=?", &s)) return -1;
  sqlite3_bind_text(s, 1, account, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(s), result = -1;
  if (rc == SQLITE_DONE) result = 0;
  else if (rc == SQLITE_ROW && sqlite3_column_bytes(s, 9) == 32 && sqlite3_column_bytes(s, 10) == 16) {
    memset(r, 0, sizeof *r);
    snprintf(r->account, sizeof r->account, "%s", sqlite3_column_text(s, 0));
    snprintf(r->home, sizeof r->home, "%s", sqlite3_column_text(s, 1));
    snprintf(r->operation, sizeof r->operation, "%s", sqlite3_column_text(s, 2));
    snprintf(r->state, sizeof r->state, "%s", sqlite3_column_text(s, 3));
    r->uid = (uint32_t)sqlite3_column_int64(s, 4);
    r->generation = (uint32_t)sqlite3_column_int64(s, 5);
    r->dev = (uint64_t)sqlite3_column_int64(s, 6);
    r->ino = (uint64_t)sqlite3_column_int64(s, 7);
    r->epoch = (uint64_t)sqlite3_column_int64(s, 8);
    memcpy(r->salt, sqlite3_column_blob(s, 9), 32);
    memcpy(r->id, sqlite3_column_blob(s, 10), 16);
    r->provisioning = sqlite3_column_int(s, 11);
    result = nh_identity_uuid_is_valid(r->account) && r->home[0] == '/' && r->generation ? 1 : -1;
  }
  sqlite3_finalize(s);
  return result;
}
static int save(nh_fscrypt *c, const record *r) {
  sqlite3_stmt *s = NULL;
  if (statement(c, "INSERT INTO homes VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(account) DO UPDATE SET home=excluded.home,operation=excluded.operation,state=excluded.state,uid=excluded.uid,generation=excluded.generation,dev=excluded.dev,ino=excluded.ino,epoch=excluded.epoch,salt=excluded.salt,key_id=excluded.key_id,provisioning=excluded.provisioning", &s)) return -1;
  sqlite3_bind_text(s, 1, r->account, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, r->home, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, r->operation, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 4, r->state, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(s, 5, r->uid);
  sqlite3_bind_int64(s, 6, r->generation);
  sqlite3_bind_int64(s, 7, (sqlite3_int64)r->dev);
  sqlite3_bind_int64(s, 8, (sqlite3_int64)r->ino);
  sqlite3_bind_int64(s, 9, (sqlite3_int64)r->epoch);
  sqlite3_bind_blob(s, 10, r->salt, 32, SQLITE_TRANSIENT);
  sqlite3_bind_blob(s, 11, r->id, 16, SQLITE_TRANSIENT);
  sqlite3_bind_int(s, 12, r->provisioning);
  int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(s);
  return rc;
}
/* Reject symlink ancestors, not just a symlink basename. All ancestors of the
 * authority and home parent must be root-controlled and not writable by users. */
static int root_directory(const char *path) {
  if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) return -1;
  int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  char copy[PATH_MAX]; strcpy(copy, path);
  char *p = NULL;
  for (char *part = strtok_r(copy, "/", &p); part && fd >= 0; part = strtok_r(NULL, "/", &p)) {
    if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
    int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(fd); fd = next;
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid || (st.st_mode & 0022))) { close(fd); return -1; }
  }
  return fd;
}
static int parent(const record *r, char name[256]) {
  char path[256]; strcpy(path, r->home);
  char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return -1;
  strcpy(name, slash + 1);
  if (slash == path) slash[1] = 0; else *slash = 0;
  return root_directory(path);
}
static int home_fd(const record *r, int root, const char *name) {
  int fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st;
  if (fd >= 0 && (fstat(fd, &st) || (uint64_t)st.st_dev != r->dev || (uint64_t)st.st_ino != r->ino ||
      st.st_uid != r->uid || (st.st_mode & 07777) != 0700 || nh_fscrypt_verify_policy(fd, r->id))) {
    close(fd); fd = -1;
  }
  return fd;
}
/* Runtime status stays outside the locked tree. It contains no salt or key;
 * readers merge it into nh_porthome_status's fscrypt object for their own UID. */
static void emit_status(const record *r) {
  int root = root_directory("/run/nostr-auth");
  if (root < 0) return;
  if (mkdirat(root, "fscrypt", 0755) && errno != EEXIST) { close(root); return; }
  int dir = openat(root, "fscrypt", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(root);
  struct stat st;
  if (dir < 0) return;
  if (fstat(dir, &st) || st.st_uid || (st.st_mode & 0022)) { close(dir); return; }
  char name[32], tmp[64], text[192];
  snprintf(name, sizeof name, "%u.json", r->uid);
  snprintf(tmp, sizeof tmp, ".%u.%ld.tmp", r->uid, (long)getpid());
  int n = snprintf(text, sizeof text, "{\"state\":\"%s\",\"generation\":%u,\"epoch\":%llu}\n",
      r->state, r->generation, (unsigned long long)r->epoch);
  int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd >= 0) {
    int ok = write(fd, text, (size_t)n) == n && !fchmod(fd, 0644) && !fsync(fd);
    close(fd);
    if (ok) (void)renameat(dir, tmp, dir, name);
    (void)unlinkat(dir, tmp, 0);
  }
  close(dir);
}
static int process_start(pid_t pid, uint64_t *out) {
  char path[64], buf[4096]; snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  char *p = strrchr(buf, ')');
  if (!p || p[1] != ' ') return -1;
  p += 2;
  /* state is field 3; starttime is field 22. comm may contain spaces/'). */
  for (unsigned field = 3; field < 22; field++) {
    p = strchr(p, ' '); if (!p) return -1; p++;
  }
  char *end = NULL; errno = 0;
  unsigned long long value = strtoull(p, &end, 10);
  if (errno || end == p || *end != ' ') return -1;
  *out = (uint64_t)value;
  return 0;
}
static int admission(nh_fscrypt *c, const record *r, pid_t pid) {
  uint64_t start;
  if (pid <= 1 || process_start(pid, &start)) return -1;
  int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
  if (pfd < 0) return -1;
  struct pollfd p = {.fd = pfd, .events = POLLIN};
  int alive = poll(&p, 1, 0) == 0;
  close(pfd);
  if (!alive) return -1;
  sqlite3_stmt *s = NULL;
  if (statement(c, "INSERT OR REPLACE INTO admissions(account,pid,start,boot) VALUES(?,?,?,?)", &s)) return -1;
  sqlite3_bind_text(s, 1, r->account, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(s, 2, pid);
  sqlite3_bind_int64(s, 3, (sqlite3_int64)start);
  sqlite3_bind_text(s, 4, c->boot, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(s);
  return rc;
}
int nh_fscrypt_required(nh_fscrypt *c, const char *id, int new_home) {
  if (!c) return 0;
  if (enter(c)) return -1;
  record r;
  int rc = load(c, id, &r);
  if (!rc && new_home) rc = c->enable_new;
  leave(c);
  return rc;
}
int nh_fscrypt_admit(nh_fscrypt *c, const nh_identity_account *account,
    const uint8_t seed[32], pid_t peer, const char *operation, int new_home) {
  if (!c) return 0;
  if (!account || !nh_identity_uuid_is_valid(account->account_id) || enter(c)) return -1;
  int rc = -1, root = -1, fd = -1, added = 0, transaction = 0;
  uint8_t *secret = MAP_FAILED, derived[16];
  record r;
  int exists = load(c, account->account_id, &r);
  if (exists < 0) goto out;
  if (!exists && !new_home) {
    /* Metadata loss is not an opt-out. Never treat an encrypted inode with no
     * authority record as an ordinary portable home. */
    memset(&r, 0, sizeof r); strcpy(r.home, account->home);
    char name[256]; root = parent(&r, name);
    if (root < 0) goto out;
    fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && nh_fscrypt_has_policy(fd) == 0) rc = 0;
    goto out;
  }
  if (!exists && !c->enable_new) { rc = 0; goto out; }
  if (c->started && !atomic_load(&c->watching)) { errno = ENOTCONN; goto out; }
  if (!seed) { errno = ENOKEY; goto out; }
  if (!exists) {
    if (!operation || !nh_identity_uuid_is_valid(operation)) goto out;
    memset(&r, 0, sizeof r);
    strcpy(r.account, account->account_id); strcpy(r.home, account->home);
    strcpy(r.operation, operation); r.uid = account->uid; r.generation = 1;
    r.provisioning = 1; strcpy(r.state, "preparing");
    if (RAND_bytes(r.salt, 32) != 1) goto out;
    char linger[512]; snprintf(linger, sizeof linger, "/var/lib/systemd/linger/%s", account->username);
    struct stat st;
    if (!lstat(linger, &st) || errno != ENOENT) { errno = EBUSY; goto out; }
  } else if (strcmp(r.home, account->home) || r.uid != account->uid ||
             strcmp(r.state, "repair-required") == 0 ||
             (r.provisioning && (!new_home || !operation || strcmp(operation, r.operation))) ||
             strcmp(r.state, "migrating") == 0) goto out;
  char name[256]; root = parent(&r, name);
  if (root < 0) goto out;
  if (!exists) {
    if ((!c->override && nh_porthome_at_rest_fd(root) != NH_AT_REST_ENCRYPTED) ||
        nh_fscrypt_probe(root)) goto out;
  } else if (!r.provisioning) {
    fd = home_fd(&r, root, name);
    if (fd < 0) { strcpy(r.state, "repair-required"); (void)save(c, &r); goto out; }
  }
  secret = mmap(NULL, 96, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (secret == MAP_FAILED || mlock(secret, 96) || madvise(secret, 96, MADV_DONTDUMP)) goto out;
  if (porthome_key_derive(seed, secret) ||
      porthome_fscrypt_master_derive(secret, r.account, r.salt, r.generation, secret + 32)) goto out;
  if (nh_fscrypt_identifier(secret + 32, derived)) goto out;
  if (exists && CRYPTO_memcmp(derived, r.id, 16)) { errno = EKEYREJECTED; goto out; }
  memcpy(r.id, derived, 16);
  if (r.epoch == INT64_MAX || exec_sql(c, "BEGIN IMMEDIATE")) goto out;
  transaction = 1;
  ++r.epoch; /* durable cancellation and ADD intent precede the kernel ioctl */
  if (admission(c, &r, peer) || save(c, &r) || exec_sql(c, "COMMIT")) goto out;
  transaction = 0;
  if (nh_fscrypt_add(root, secret + 32, derived)) goto out;
  added = 1;
  if (nh_fscrypt_key_status(root, derived) != NH_FSCRYPT_PRESENT) goto out;
  if (CRYPTO_memcmp(derived, r.id, 16)) { errno = EKEYREJECTED; goto out; }
  if (exists && !r.provisioning) strcpy(r.state, "unlocked");
  if (save(c, &r)) goto out;
  emit_status(&r);
  rc = 1;
out:;
  int saved = errno;
  if (transaction) (void)exec_sql(c, "ROLLBACK");
  /* A mismatch is a separate key, not the active account key. Never revoke an
   * existing account's key on a failed new login while another session uses it. */
  if (rc < 0 && added && (!exists || CRYPTO_memcmp(derived, r.id, 16)))
    (void)nh_fscrypt_remove(root, derived);
  if (secret != MAP_FAILED) { OPENSSL_cleanse(secret, 96); munlock(secret, 96); munmap(secret, 96); }
  if (fd >= 0) close(fd);
  if (root >= 0) close(root);
  leave(c); wake(c); errno = saved;
  return rc;
}
int nh_fscrypt_stage(nh_fscrypt *c, const char *id, int fd, int initialize) {
  if (!c) return 0;
  if (enter(c)) return -1;
  record r; int found = load(c, id, &r), rc = -1;
  if (!found) rc = 0;
  else if (found == 1 && (r.provisioning || !initialize) &&
           strcmp(r.state, "repair-required") && strcmp(r.state, "migrating")) {
    struct stat st;
    if (!fstat(fd, &st) && (!initialize || (!r.ino && !nh_fscrypt_set_policy(fd, r.id))) &&
        !nh_fscrypt_verify_policy(fd, r.id) &&
        (initialize || ((uint64_t)st.st_dev == r.dev && (uint64_t)st.st_ino == r.ino))) {
      if (initialize) {
        r.dev = (uint64_t)st.st_dev; r.ino = (uint64_t)st.st_ino;
        rc = save(c, &r);
      } else rc = 0;
    }
  }
  leave(c);
  return rc;
}
int nh_fscrypt_finished(nh_fscrypt *c, const char *id, int success) {
  if (!c) return 0;
  if (enter(c)) return -1;
  record r; int found = load(c, id, &r), rc = -1;
  if (!found) rc = 0;
  else if (found == 1) {
    char name[256]; int root = parent(&r, name), fd = -1;
    if (success && root >= 0) fd = home_fd(&r, root, name);
    if (fd < 0) success = 0;
    if (fd >= 0) close(fd);
    if (root >= 0) close(root);
    r.provisioning = 0;
    strcpy(r.state, success ? "unlocked" : "repair-required");
    rc = save(c, &r);
    if (!rc) emit_status(&r);
    if (!success) rc = -1;
  }
  leave(c); wake(c);
  return rc;
}

/* Watcher: subscribe before the first snapshot. D-Bus observation is never
 * inferred from process names or inactivity timeouts. Pending PAM admissions
 * are independent of sockets and bridge the gap before pam_systemd. */
static int changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
  (void)m; (void)userdata; (void)error;
  return 0;
}
static int sessions(nh_fscrypt *c, uint32_t uid) {
  sd_bus_message *reply = NULL;
  int rc = sd_bus_call_method(c->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager", "ListSessions", NULL, &reply, "");
  if (rc < 0) return -1;
  int count = 0;
  if (sd_bus_message_enter_container(reply, 'a', "(susso)") < 0) count = -1;
  else {
    const char *id, *user, *seat, *path; uint32_t session_uid;
    while ((rc = sd_bus_message_read(reply, "(susso)", &id, &session_uid, &user, &seat, &path)) > 0)
      if (session_uid == uid) count++;
    if (rc < 0) count = -1;
  }
  sd_bus_message_unref(reply);
  return count;
}
static int manager_inactive(nh_fscrypt *c, uint32_t uid) {
  char unit[64]; snprintf(unit, sizeof unit, "user@%u.service", uid);
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int rc = sd_bus_call_method(c->bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
      "org.freedesktop.systemd1.Manager", "GetUnit", &error, &reply, "s", unit);
  int inactive = 0;
  if (rc < 0) inactive = sd_bus_error_has_name(&error, "org.freedesktop.systemd1.NoSuchUnit");
  else {
    const char *path = NULL;
    char *state = NULL;
    if (sd_bus_message_read(reply, "o", &path) > 0 &&
        sd_bus_get_property_string(c->bus, "org.freedesktop.systemd1", path,
          "org.freedesktop.systemd1.Unit", "ActiveState", NULL, &state) >= 0)
      inactive = !strcmp(state, "inactive") || !strcmp(state, "failed");
    if (inactive) {
      uint32_t main_pid = UINT32_MAX;
      inactive = sd_bus_get_property_trivial(c->bus, "org.freedesktop.systemd1", path,
          "org.freedesktop.systemd1.Service", "MainPID", NULL, 'u', &main_pid) >= 0 && main_pid == 0;
    }
    free(state);
  }
  sd_bus_message_unref(reply); sd_bus_error_free(&error);
  return inactive;
}
static int pending(nh_fscrypt *c, const record *r) {
  sqlite3_stmt *s = NULL;
  if (statement(c, "SELECT rowid,pid,start,boot FROM admissions WHERE account=?", &s)) return -1;
  sqlite3_bind_text(s, 1, r->account, -1, SQLITE_TRANSIENT);
  int count = 0, step;
  while ((step = sqlite3_step(s)) == SQLITE_ROW) {
    pid_t pid = (pid_t)sqlite3_column_int64(s, 1);
    uint64_t start = 0;
    int retire = strcmp((const char *)sqlite3_column_text(s, 3), c->boot) != 0;
    if (!retire) {
      int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
      if (pfd < 0) retire = errno == ESRCH;
      else {
        struct pollfd p = {.fd = pfd, .events = POLLIN};
        retire = poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
        close(pfd);
      }
      if (!retire && !process_start(pid, &start) && start != (uint64_t)sqlite3_column_int64(s, 2)) retire = 1;
      if (!retire) {
        char *session = NULL; uid_t owner;
        /* sd-login reads logind's published runtime state. Only this exact
         * originating process's session can retire its admission. */
        if (sd_pid_get_session(pid, &session) >= 0 && sd_session_get_uid(session, &owner) >= 0 && owner == r->uid)
          retire = 1;
        free(session);
      }
    }
    if (retire) {
      sqlite3_stmt *d = NULL;
      if (statement(c, "DELETE FROM admissions WHERE rowid=?", &d)) { count = -1; break; }
      sqlite3_bind_int64(d, 1, sqlite3_column_int64(s, 0));
      int ok = sqlite3_step(d) == SQLITE_DONE;
      sqlite3_finalize(d);
      if (!ok) { count = -1; break; }
    } else count++;
  }
  if (step != SQLITE_DONE) count = -1;
  sqlite3_finalize(s);
  return count;
}
static int read_journal(nh_fscrypt *c, const char *id, record *old, record *next, char phase[16]);
static int reconcile(nh_fscrypt *c) {
  if (enter(c)) return 0;
  sqlite3_stmt *s = NULL;
  int busy = 0;
  if (statement(c, "SELECT account FROM homes", &s)) { leave(c); return 0; }
  char (*ids)[37] = NULL;
  size_t count = 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    void *more = realloc(ids, (count + 1) * sizeof *ids);
    if (!more) { free(ids); sqlite3_finalize(s); leave(c); return 0; }
    ids = more;
    snprintf(ids[count++], 37, "%s", sqlite3_column_text(s, 0));
  }
  sqlite3_finalize(s); leave(c);
  for (size_t i = 0; i < count; i++) {
    if (enter(c)) continue;
    record r;
    if (load(c, ids[i], &r) != 1) { leave(c); continue; }
    emit_status(&r);
    /* Removing stale admissions requires fresh logind + systemd observations.
     * Process exit by itself never proves that its session has stopped. */
    int admissions = pending(c, &r);
    leave(c);
    if (admissions != 0) continue;
    /* Bus calls never hold the mutation lock. Snapshot after retirement, then
     * revalidate the epoch under that lock: a newer admission invalidates it. */
    nh_fscrypt_lifecycle life = {.epoch = r.epoch, .sessions = sessions(c, r.uid),
      .manager_inactive = manager_inactive(c, r.uid), .provisioning = r.provisioning,
      .maintenance = 0, .admissions = (unsigned)admissions};
    if (!nh_fscrypt_lock_eligible(&life, r.epoch) || enter(c)) continue;
    record fresh;
    if (load(c, r.account, &fresh) != 1 || fresh.epoch != r.epoch ||
        fresh.provisioning || pending(c, &fresh) != 0 ||
        CRYPTO_memcmp(fresh.id, r.id, 16)) { leave(c); continue; }
    r = fresh;
    char name[256]; int root = parent(&r, name);
    if (root < 0) { leave(c); continue; }
    /* The authority process lock excludes a live maintenance process. On
     * daemon restart an interrupted journal stays blocked for explicit resume,
     * but its recorded old/new claims must not remain unlocked indefinitely. */
    if (!strcmp(r.state, "migrating")) {
      record old = r, next = r; char phase[16];
      if (read_journal(c, r.account, &old, &next, phase) == 1) {
        if (old.generation && nh_fscrypt_remove(root, old.id) != NH_FSCRYPT_ABSENT) busy = 1;
        if (nh_fscrypt_remove(root, next.id) != NH_FSCRYPT_ABSENT) busy = 1;
      }
      close(root); leave(c); continue;
    }
    nh_fscrypt_key_state result = nh_fscrypt_remove(root, r.id);
    close(root);
    const char *state = result == NH_FSCRYPT_ABSENT ? "locked" : "locking";
    if (strcmp(r.state, "repair-required") && strcmp(r.state, state)) {
      strcpy(r.state, state);
      if (!save(c, &r)) emit_status(&r);
      syslog(result == NH_FSCRYPT_ABSENT ? LOG_INFO : LOG_WARNING,
          "fscrypt account=%s epoch=%llu state=%s%s", r.account,
          (unsigned long long)r.epoch, state,
          result == NH_FSCRYPT_BUSY ? " (open files or other key claims prevent complete removal)" : "");
    }
    if (result != NH_FSCRYPT_ABSENT) busy = 1;
    leave(c);
  }
  free(ids);
  return busy;
}
static void *watcher(void *arg) {
  nh_fscrypt *c = arg;
  int busy = reconcile(c);
  while (!atomic_load(&c->stop)) {
    int processed, any = 0;
    while ((processed = sd_bus_process(c->bus, NULL)) > 0) any = 1;
    if (processed < 0) break;
    if (any) busy = reconcile(c);
    /* pidfds wake failed pre-session handoffs without polling processes. */
    struct pollfd p[4098]; size_t count = 2;
    int exited = 0;
    p[0] = (struct pollfd){.fd = sd_bus_get_fd(c->bus), .events = POLLIN};
    p[1] = (struct pollfd){.fd = c->event, .events = POLLIN};
    if (!enter(c)) {
      sqlite3_stmt *s = NULL;
      if (!statement(c, "SELECT DISTINCT pid FROM admissions", &s)) {
        while (count < sizeof p / sizeof p[0] && sqlite3_step(s) == SQLITE_ROW) {
          int fd = (int)syscall(SYS_pidfd_open, (pid_t)sqlite3_column_int64(s, 0), 0);
          if (fd >= 0) p[count++] = (struct pollfd){.fd = fd, .events = POLLIN};
          else if (errno == ESRCH) exited = 1;
        }
        sqlite3_finalize(s);
      }
      leave(c);
    }
    /* Only incomplete key removal has a timer. Session/user-manager lifecycle
     * is driven by bus signals and peer exit, never periodic logind polling. */
    int result = poll(p, count, exited ? 0 : (busy ? 1000 : -1));
    for (size_t i = 2; i < count; i++) close(p[i].fd);
    if (result < 0 && errno == EINTR) continue;
    if (result < 0) break;
    if (p[1].revents) { uint64_t value; (void)read(c->event, &value, sizeof value); }
    int rc;
    while ((rc = sd_bus_process(c->bus, NULL)) > 0) {}
    if (rc < 0) {
      /* No complete observation => no removal. A restart reconnects and
       * reconciles durable records; never turn a bus outage into a lock. */
      syslog(LOG_ERR, "fscrypt watcher lost system bus; locking suspended");
      break;
    }
    busy = reconcile(c);
  }
  atomic_store(&c->watching, 0);
  return NULL;
}
static int open_context(const char *dir, int enable, int override, int watch, nh_fscrypt **out) {
  if (!out || geteuid()) return -1;
  *out = NULL;
  int root = root_directory(dir);
  if (root < 0) return -1;
  if (mkdirat(root, "fscrypt", 0700) && errno != EEXIST) { close(root); return -1; }
  int private = openat(root, "fscrypt", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(root); root = private;
  struct stat st;
  if (root < 0 || fstat(root, &st) || st.st_uid || (st.st_mode & 0077)) {
    if (root >= 0) close(root);
    return -1;
  }
  nh_fscrypt *c = calloc(1, sizeof *c);
  if (!c) { close(root); return -1; }
  c->event = c->lock = -1; c->enable_new = !!enable; c->override = !!override;
  pthread_mutex_init(&c->mutex, NULL);
  int dbfd = openat(root, "state.db", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (dbfd < 0 || fstat(dbfd, &st) || st.st_uid || st.st_nlink != 1 || !S_ISREG(st.st_mode) || (st.st_mode & 0077)) {
    if (dbfd >= 0) close(dbfd);
    goto fail;
  }
  close(dbfd);
  char dbpath[PATH_MAX];
  if (snprintf(dbpath, sizeof dbpath, "%s/fscrypt/state.db", dir) >= (int)sizeof dbpath) goto fail;
  c->lock = openat(root, "state.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (c->lock < 0 || fstat(c->lock, &st) || st.st_uid || st.st_nlink != 1 || !S_ISREG(st.st_mode) || (st.st_mode & 0077)) goto fail;
  if (sqlite3_open_v2(dbpath, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) goto fail;
  if (exec_sql(c, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS homes(account TEXT PRIMARY KEY,home TEXT NOT NULL,operation TEXT NOT NULL,state TEXT NOT NULL,uid INTEGER NOT NULL,generation INTEGER NOT NULL,dev INTEGER NOT NULL,ino INTEGER NOT NULL,epoch INTEGER NOT NULL,salt BLOB NOT NULL,key_id BLOB NOT NULL,provisioning INTEGER NOT NULL); CREATE TABLE IF NOT EXISTS admissions(account TEXT NOT NULL,pid INTEGER NOT NULL,start INTEGER NOT NULL,boot TEXT NOT NULL,PRIMARY KEY(account,pid,start,boot));")) goto fail;
  int boot = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (boot < 0) goto fail;
  ssize_t n = read(boot, c->boot, sizeof c->boot - 1); close(boot);
  if (n < 36) goto fail;
  c->boot[36] = 0;
  c->event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (c->event < 0 || sd_bus_open_system(&c->bus) < 0 || sd_bus_set_method_call_timeout(c->bus, 3000000) < 0) goto fail;
  if (sd_bus_add_match(c->bus, NULL, "type='signal',sender='org.freedesktop.login1'", changed, c) < 0 ||
      sd_bus_add_match(c->bus, NULL, "type='signal',sender='org.freedesktop.systemd1'", changed, c) < 0 ||
      sd_bus_call_method(c->bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
          "org.freedesktop.systemd1.Manager", "Subscribe", NULL, NULL, "") < 0) goto fail;
  if (exec_sql(c, "CREATE TABLE IF NOT EXISTS maintenance(account TEXT PRIMARY KEY,document TEXT NOT NULL)")) goto fail;
  /* A crash during provisioning cannot be replayed as an ordinary login. Keep
   * its encrypted stage for explicit repair but reconcile any orphan key. */
  if (exec_sql(c, "UPDATE homes SET state='repair-required',provisioning=0 WHERE provisioning<>0")) goto fail;
  if (watch) {
    atomic_store(&c->watching, 1);
    if (pthread_create(&c->thread, NULL, watcher, c)) goto fail;
    c->started = 1;
  }
  close(root); *out = c; return 0;
fail:
  close(root); sd_bus_unref(c->bus);
  if (c->db) sqlite3_close(c->db);
  if (c->event >= 0) close(c->event);
  if (c->lock >= 0) close(c->lock);
  pthread_mutex_destroy(&c->mutex); free(c);
  return -1;
}
int nh_fscrypt_open(const char *dir, int enable, int override, nh_fscrypt **out) {
  return open_context(dir, enable, override, 1, out);
}
/* Maintenance journals contain only public derivation metadata and exact
 * inode evidence. The original remains reachable until both databases record
 * the replacement; replay classifies the two names by inode, never by name. */
static json_t *metadata(const record *r) {
  char salt[65], id[65]; uint8_t padded[32] = {0};
  porthome_hex64(r->salt, salt); memcpy(padded, r->id, 16); porthome_hex64(padded, id);
  return json_pack("{s:I,s:s,s:s,s:I,s:I}", "generation", (json_int_t)r->generation,
      "salt", salt, "identifier", id, "device", (json_int_t)r->dev, "inode", (json_int_t)r->ino);
}
static int read_metadata(json_t *obj, record *r) {
  json_t *g = json_object_get(obj, "generation"), *d = json_object_get(obj, "device"), *i = json_object_get(obj, "inode");
  const char *salt = json_string_value(json_object_get(obj, "salt"));
  const char *id = json_string_value(json_object_get(obj, "identifier"));
  uint8_t padded[32], zero[16] = {0};
  if (!json_is_integer(g) || json_integer_value(g) < 0 || json_integer_value(g) > UINT32_MAX ||
      !json_is_integer(d) || json_integer_value(d) < 0 || !json_is_integer(i) || json_integer_value(i) < 0 ||
      !salt || !id || porthome_from_hex64(salt, r->salt) || porthome_from_hex64(id, padded) ||
      memcmp(padded + 16, zero, 16)) return -1;
  r->generation = (uint32_t)json_integer_value(g); r->dev = (uint64_t)json_integer_value(d); r->ino = (uint64_t)json_integer_value(i);
  memcpy(r->id, padded, 16);
  return 0;
}
static int journal(nh_fscrypt *c, const record *old, const record *next, const char *phase) {
  json_t *o = json_pack("{s:i,s:s,s:o,s:o}", "version", 1, "phase", phase, "old", metadata(old), "next", metadata(next));
  if (!o) return -1;
  char *text = json_dumps(o, JSON_COMPACT | JSON_SORT_KEYS); json_decref(o);
  if (!text) return -1;
  sqlite3_stmt *s = NULL;
  int rc = -1;
  if (!statement(c, "INSERT OR REPLACE INTO maintenance VALUES(?,?)", &s)) {
    sqlite3_bind_text(s, 1, old->account, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, text, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
  }
  sqlite3_finalize(s); free(text); return rc;
}
static int read_journal(nh_fscrypt *c, const char *id, record *old, record *next, char phase[16]) {
  sqlite3_stmt *s = NULL;
  if (statement(c, "SELECT document FROM maintenance WHERE account=?", &s)) return -1;
  sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
  int step = sqlite3_step(s), rc = -1;
  if (step == SQLITE_DONE) rc = 0;
  else if (step == SQLITE_ROW && sqlite3_column_bytes(s, 0) < 4096) {
    json_error_t error;
    json_t *o = json_loads((const char *)sqlite3_column_text(s, 0), JSON_REJECT_DUPLICATES, &error);
    const char *p = json_string_value(json_object_get(o, "phase"));
    if (o && json_integer_value(json_object_get(o, "version")) == 1 && p &&
        (!strcmp(p, "copying") || !strcmp(p, "copied") || !strcmp(p, "committed")) &&
        !read_metadata(json_object_get(o, "old"), old) && !read_metadata(json_object_get(o, "next"), next)) {
      strcpy(phase, p); rc = 1;
    }
    json_decref(o);
  }
  sqlite3_finalize(s); return rc;
}
static int seed_key(int root, const record *r, const uint8_t seed[32], int install, uint8_t id[16]) {
  uint8_t *memory = mmap(NULL, 96, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) return -1;
  int rc = -1;
  if (!mlock(memory, 96) && !madvise(memory, 96, MADV_DONTDUMP) &&
      !porthome_key_derive(seed, memory) &&
      !porthome_fscrypt_master_derive(memory, r->account, r->salt, r->generation, memory + 32) &&
      !(install ? nh_fscrypt_add(root, memory + 32, id) : nh_fscrypt_identifier(memory + 32, id))) {
    rc = !install || !CRYPTO_memcmp(id, r->id, 16) ? 0 : -1;
    if (rc && install) (void)nh_fscrypt_remove(root, id);
  }
  OPENSSL_cleanse(memory, 96); munlock(memory, 96); munmap(memory, 96);
  return rc;
}
static int inode_at(int root, const char *name, uint64_t dev, uint64_t ino) {
  struct stat st;
  if (fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : -1;
  return S_ISDIR(st.st_mode) && (uint64_t)st.st_dev == dev && (uint64_t)st.st_ino == ino ? 1 : -1;
}
/* Offline maintenance also excludes processes outside logind/user@. Root
 * operators must stop scheduled jobs and other privileged writers. Fail closed
 * on unreadable process metadata; open FDs/mappings into either tree block it. */
static int tree_path(const char *path, const char *tree) {
  size_t n = strlen(tree);
  return !strncmp(path, tree, n) && (!path[n] || path[n] == '/' || path[n] == ' ');
}
static int writers_quiescent(uint32_t uid, const char *home, const char *recovery) {
  DIR *proc = opendir("/proc");
  if (!proc) return 0;
  int ok = 1; struct dirent *entry;
  while (ok && (entry = readdir(proc))) {
    char *end; long pid = strtol(entry->d_name, &end, 10);
    if (*end || pid <= 0 || pid == getpid()) continue;
    char path[128], line[8192];
    snprintf(path, sizeof path, "/proc/%ld/status", pid);
    FILE *f = fopen(path, "re");
    if (!f) { if (errno != ENOENT) ok = 0; continue; }
    int saw_uid = 0;
    while (fgets(line, sizeof line, f)) {
      unsigned real, effective, saved, fs;
      if (sscanf(line, "Uid:\t%u\t%u\t%u\t%u", &real, &effective, &saved, &fs) == 4) {
        saw_uid = 1;
        if (real == uid || effective == uid || saved == uid || fs == uid) ok = 0;
      }
    }
    if (ferror(f) || !saw_uid) ok = 0;
    fclose(f);
    if (!ok) break;
    snprintf(path, sizeof path, "/proc/%ld/fd", pid);
    DIR *fds = opendir(path);
    if (!fds) { if (errno != ENOENT) ok = 0; continue; }
    struct dirent *fd;
    while (ok && (fd = readdir(fds))) {
      if (fd->d_name[0] == '.') continue;
      ssize_t n = readlinkat(dirfd(fds), fd->d_name, line, sizeof line - 1);
      if (n < 0) { if (errno != ENOENT) ok = 0; continue; }
      line[n] = 0;
      if (tree_path(line, home) || tree_path(line, recovery)) ok = 0;
    }
    closedir(fds);
    snprintf(path, sizeof path, "/proc/%ld/maps", pid);
    f = fopen(path, "re");
    if (!f) { if (errno != ENOENT) ok = 0; continue; }
    while (ok && fgets(line, sizeof line, f)) {
      char *p = strchr(line, '/');
      if (p) { p[strcspn(p, "\n")] = 0; if (tree_path(p, home) || tree_path(p, recovery)) ok = 0; }
    }
    if (ferror(f)) ok = 0;
    fclose(f);
  }
  closedir(proc);
  if (!ok) errno = EBUSY;
  return ok;
}
static int migrate(nh_fscrypt *c, nh_identity_store *store, const nh_identity_account *account,
    const uint8_t old_seed[32], const uint8_t new_seed[32]) {
  if (enter(c)) return -1;
  record current = {0}, old, next;
  int exists = load(c, account->account_id, &current), rc = -1, root = -1, src = -1, dst = -1, recovery = -1;
  int old_added = 0, next_added = 0;
  uint8_t old_id[16], next_id[16];
  if (exists < 0) goto out;
  nh_identity_portable_home portable;
  nh_identity_operation_state op;
  if (nh_identity_portable_home_get(store, account->account_id, &portable) != NH_IDENTITY_OK ||
      strcmp(portable.state, "installed") ||
      nh_identity_operation_get(store, portable.operation_id, &op) != NH_IDENTITY_OK) goto out;
  if (!exists) {
    strcpy(current.account, account->account_id); strcpy(current.home, account->home);
    strcpy(current.operation, portable.operation_id); current.uid = account->uid;
    current.dev = op.installed_home.filesystem_device; current.ino = op.installed_home.filesystem_inode;
  }
  if (strcmp(current.home, account->home) || current.uid != account->uid || current.provisioning) goto out;
  old = next = current;
  char phase[16] = {0};
  int resumed = read_journal(c, account->account_id, &old, &next, phase);
  if (resumed < 0 || (resumed && strcmp(current.state, "migrating"))) goto out;
  int pending_count = pending(c, &current);
  if (pending_count != 0 || sessions(c, account->uid) != 0 || !manager_inactive(c, account->uid)) { errno = EBUSY; goto out; }
  char name[256], stage[96]; root = parent(&current, name);
  if (root < 0) goto out;
  if ((!c->override && nh_porthome_at_rest_fd(root) != NH_AT_REST_ENCRYPTED) || nh_fscrypt_probe(root)) goto out;
  if (!resumed) {
    if (current.generation == UINT32_MAX || current.epoch == INT64_MAX ||
        (exists && strcmp(current.state, "locked") && strcmp(current.state, "unlocked") && strcmp(current.state, "locking"))) goto out;
    next.generation = current.generation + 1; next.dev = next.ino = 0;
    if (!exists && RAND_bytes(next.salt, 32) != 1) goto out;
    if (!exists) memcpy(old.salt, next.salt, 32);
    strcpy(phase, "copying");
  }
  if (!next.generation || next.generation != old.generation + 1) goto out;
  snprintf(stage, sizeof stage, ".nostr-fscrypt-%s-%u", current.account, next.generation);
  char recovery_path[512];
  const char *slash = strrchr(current.home, '/');
  snprintf(recovery_path, sizeof recovery_path, "%.*s/%s", (int)(slash - current.home), current.home, stage);
  if (!writers_quiescent(current.uid, current.home, recovery_path)) goto out;
  if (!resumed) {
    if (seed_key(root, &next, new_seed, 0, next.id)) goto out;
    if (inode_at(root, name, old.dev, old.ino) != 1 || inode_at(root, stage, 0, 0) != 0) goto out;
    src = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (src < 0 || (old.generation ? nh_fscrypt_verify_policy(src, old.id) : nh_fscrypt_has_policy(src) != 0)) goto out;
    nh_identity_home_evidence found;
    if (nh_identity_home_validate(store, account->account_id, &op.installed_home, &found) != NH_IDENTITY_OK) goto out;
    current.generation = old.generation ? old.generation : 1;
    current.epoch++;
    strcpy(current.state, "migrating");
    if (exec_sql(c, "BEGIN IMMEDIATE")) goto out;
    if (save(c, &current) || journal(c, &old, &next, phase) || exec_sql(c, "COMMIT")) {
      (void)exec_sql(c, "ROLLBACK"); goto out;
    }
  }
  /* The user-owned old/new tree is always below a root-only recovery parent. */
  if (mkdirat(root, stage, 0700) && errno != EEXIST) goto out;
  recovery = openat(root, stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat recovery_stat;
  if (recovery < 0 || fstat(recovery, &recovery_stat) || recovery_stat.st_uid ||
      (recovery_stat.st_mode & 07777) != 0700 || fsync(root)) goto out;
  /* Both identifiers and derivation metadata are durable before either ADD. */
  if (old.generation) {
    if (seed_key(root, &old, old_seed, 1, old_id)) goto out;
    old_added = 1;
  }
  if (seed_key(root, &next, new_seed, 1, next_id)) goto out;
  next_added = 1;
  if (!strcmp(phase, "copying")) {
    if (inode_at(root, name, old.dev, old.ino) != 1) goto out;
    int stage_exists = inode_at(recovery, "tree", next.dev, next.ino);
    if (stage_exists < 0) goto out; /* unrecorded/foreign inode: preserve for repair */
    if (stage_exists == 1) {
      dst = openat(recovery, "tree", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (dst < 0 || nh_fscrypt_verify_policy(dst, next.id) || nh_fscrypt_remove_tree(dst)) goto out;
      close(dst); dst = -1;
      if (unlinkat(recovery, "tree", AT_REMOVEDIR) || fsync(recovery)) goto out;
    }
    if (mkdirat(recovery, "tree", 0700)) goto out;
    dst = openat(recovery, "tree", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (dst < 0 || nh_fscrypt_set_policy(dst, next.id) || fstat(dst, &st) || fsync(dst) || fsync(root)) goto out;
    next.dev = (uint64_t)st.st_dev; next.ino = (uint64_t)st.st_ino;
    if (journal(c, &old, &next, "copying")) goto out;
    if (src < 0) src = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (src < 0 || (old.generation ? nh_fscrypt_verify_policy(src, old.id) : nh_fscrypt_has_policy(src) != 0) ||
        nh_fscrypt_copy_tree(src, dst) || nh_fscrypt_verify_policy(dst, next.id)) goto out;
    close(src); src = -1; close(dst); dst = -1;
    if (journal(c, &old, &next, "copied")) goto out;
    strcpy(phase, "copied");
  }
  if (!strcmp(phase, "copied")) {
    if (inode_at(root, name, old.dev, old.ino) == 1 && inode_at(recovery, "tree", next.dev, next.ino) == 1) {
      if (!writers_quiescent(current.uid, current.home, recovery_path) ||
          syscall(SYS_renameat2, root, name, recovery, "tree", RENAME_EXCHANGE) ||
          fsync(root) || fsync(recovery)) goto out;
    }
    if (inode_at(root, name, next.dev, next.ino) != 1 || inode_at(recovery, "tree", old.dev, old.ino) != 1) goto out;
    nh_identity_home_evidence before = {old.dev, old.ino}, after = {next.dev, next.ino};
    dst = home_fd(&next, root, name);
    if (dst < 0 || nh_identity_portable_home_replace_evidence(store, current.account, &before, &after) != NH_IDENTITY_OK) goto out;
    close(dst); dst = -1;
    next.epoch = current.epoch; next.provisioning = 0; strcpy(next.state, "migrating");
    if (exec_sql(c, "BEGIN IMMEDIATE")) goto out;
    if (save(c, &next) || journal(c, &old, &next, "committed") || exec_sql(c, "COMMIT")) {
      (void)exec_sql(c, "ROLLBACK"); goto out;
    }
    strcpy(phase, "committed");
  }
  if (inode_at(root, name, next.dev, next.ino) != 1) goto out;
  dst = home_fd(&next, root, name);
  if (dst < 0) goto out;
  close(dst); dst = -1;
  int old_exists = inode_at(recovery, "tree", old.dev, old.ino);
  if (old_exists < 0) goto out;
  if (!writers_quiescent(current.uid, current.home, recovery_path)) goto out;
  /* Check for busy keys before deleting any original data. Re-add only after
   * complete eviction so deletion can traverse encrypted filenames. */
  if ((old_added && nh_fscrypt_remove(root, old.id) != NH_FSCRYPT_ABSENT) ||
      nh_fscrypt_remove(root, next.id) != NH_FSCRYPT_ABSENT) goto out;
  if (old_added && seed_key(root, &old, old_seed, 1, old_id)) goto out;
  if (old_exists) {
    src = openat(recovery, "tree", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (src < 0 || nh_fscrypt_remove_tree(src)) goto out;
    close(src); src = -1;
    if (unlinkat(recovery, "tree", AT_REMOVEDIR) || fsync(recovery)) goto out;
  }
  if ((old_added && nh_fscrypt_remove(root, old.id) != NH_FSCRYPT_ABSENT) ||
      nh_fscrypt_remove(root, next.id) != NH_FSCRYPT_ABSENT) {
    fprintf(stderr, "fscrypt: partial lock; close open files and resume maintenance\n"); goto out;
  }
  old_added = next_added = 0;
  next.epoch = current.epoch; next.provisioning = 0; strcpy(next.state, "locked");
  sqlite3_stmt *del = NULL;
  if (exec_sql(c, "BEGIN IMMEDIATE")) goto out;
  if (save(c, &next) || statement(c, "DELETE FROM maintenance WHERE account=?", &del)) { (void)exec_sql(c, "ROLLBACK"); goto out; }
  sqlite3_bind_text(del, 1, current.account, -1, SQLITE_TRANSIENT);
  int deleted = sqlite3_step(del) == SQLITE_DONE; sqlite3_finalize(del);
  if (!deleted || exec_sql(c, "COMMIT")) { (void)exec_sql(c, "ROLLBACK"); goto out; }
  emit_status(&next);
  if (unlinkat(root, stage, AT_REMOVEDIR) || fsync(root)) goto out;
  rc = 0;
out:
  if (recovery >= 0) close(recovery);
  if (src >= 0) close(src);
  if (dst >= 0) close(dst);
  if (root >= 0) {
    if (old_added) (void)nh_fscrypt_remove(root, old_id);
    if (next_added) (void)nh_fscrypt_remove(root, next_id);
    close(root);
  }
  leave(c);
  return rc;
}

int nh_fscrypt_maintenance_main(int argc, char **argv) {
  if (argc < 5 || argc > 7 || geteuid()) {
    fprintf(stderr, "usage (root, authd stopped): nostr-authd --fscrypt-migrate AUTHORITY_DIR ACCOUNT_UUID OLD_SEED_FD [NEW_SEED_FD] [--allow-without-luks]\n");
    return 2;
  }
  int override = !strcmp(argv[argc - 1], "--allow-without-luks");
  if (override) argc--;
  if (argc != 5 && argc != 6) return 2;
  uint8_t *seeds = mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (seeds == MAP_FAILED) return 1;
  int rc = 1;
  nh_identity_store *store = NULL; nh_fscrypt *c = NULL;
  if (mlock(seeds, 64) || madvise(seeds, 64, MADV_DONTDUMP)) goto done;
  for (int i = 0; i < (argc == 6 ? 2 : 1); i++) {
    char *end = NULL; errno = 0; long fd = strtol(argv[4 + i], &end, 10);
    if (errno || end == argv[4+i] || *end || fd < 0 || fd > INT_MAX || isatty((int)fd)) goto done;
    size_t n = 0;
    while (n < 32) {
      ssize_t got = read((int)fd, seeds + 32*i + n, 32 - n);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) goto done;
      n += (size_t)got;
    }
  }
  if (argc == 5) memcpy(seeds + 32, seeds, 32);
  nh_identity_config cfg; nh_identity_config_defaults(&cfg);
  if (snprintf(cfg.authority_path, sizeof cfg.authority_path, "%s/authority.db", argv[2]) >= (int)sizeof cfg.authority_path ||
      snprintf(cfg.projection_path, sizeof cfg.projection_path, "%s/nss.db", argv[2]) >= (int)sizeof cfg.projection_path) goto done;
  nh_identity_store_options options = {.config = &cfg};
  /* The identity store's exclusive process lock excludes the running broker.
   * Pending admissions and live sessions/managers are checked independently. */
  if (nh_identity_store_open(&options, &store) != NH_IDENTITY_OK) goto done;
  nh_identity_account account;
  if (nh_identity_store_lookup_by_id(store, argv[3], &account) != NH_IDENTITY_OK ||
      open_context(argv[2], 0, override, 0, &c)) goto done;
  if (!migrate(c, store, &account, seeds, seeds + 32)) rc = 0;
done:
  if (c) nh_fscrypt_close(c);
  if (store) nh_identity_store_close(store);
  OPENSSL_cleanse(seeds, 64); munlock(seeds, 64); munmap(seeds, 64);
  fprintf(stderr, rc ? "fscrypt maintenance refused/incomplete; original or journaled replacement retained. Resolve cause and resume with the same seeds; do not delete stages.\n" :
      "fscrypt maintenance complete and locked. Deleted plaintext may remain in free blocks/backups; this is NOT secure erasure.\n");
  return rc;
}

void nh_fscrypt_close(nh_fscrypt *c) {
  if (!c) return;
  if (c->started) { atomic_store(&c->stop, 1); wake(c); pthread_join(c->thread, NULL); }
  sd_bus_unref(c->bus); sqlite3_close(c->db); close(c->event); close(c->lock);
  pthread_mutex_destroy(&c->mutex); free(c);
}
