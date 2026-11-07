#!/usr/bin/env python3
"""Root host control plane for the single-writer Signet deployment.

No shell evaluation of site configuration. Local store commands always run as
UID 10001 in the network-isolated ops container. Failures leave state in place.
"""
import argparse
import configparser
import contextlib
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request

HOME = Path('/opt/signet')
VOLUME = Path('/srv/signet')
STOP = VOLUME / 'last-stop.json'
DIGEST = re.compile(r'^[a-z0-9./:_-]+@sha256:[0-9a-f]{64}$')
PROFILES = {'bootstrap', 'passkeys', 'ctap'}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def run(args, capture=False, **kw):
    # Never let a caller's DOCKER_HOST, COMPOSE_FILE or secret env alter custody.
    env = {'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
           'HOME': '/root', 'DOCKER_CONFIG': '/opt/signet/secrets/docker'}
    return subprocess.run([str(a) for a in args], check=True, text=True,
                          stdout=subprocess.PIPE if capture else None, env=env, **kw)


def atomic_json(path, value):
    tmp = path.with_name(path.name + '.tmp')
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(value, out, indent=2, sort_keys=True)
            out.write('\n')
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, path)
        dfd = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    finally:
        tmp.unlink(missing_ok=True)


def owned(path, uid, mode, directory=False):
    s = path.lstat()
    require((stat.S_ISDIR(s.st_mode) if directory else stat.S_ISREG(s.st_mode))
            and s.st_uid == uid and stat.S_IMODE(s.st_mode) == mode,
            f'unsafe ownership/type/mode: {path}; expected uid={uid} mode={mode:o}')
    if uid == 10001:
        require(s.st_gid == 10001, f'wrong group: {path}')


def env_file(path, allowed):
    values = {}
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        key, sep, value = line.partition('=')
        require(sep and key in allowed and key not in values and
                not re.search(r'[\s\x00\x24\x60\x22\x27]', value),
                f'invalid or unsupported setting in {path}')
        values[key] = value
    return values


def require_jsonschema():
    try:
        import jsonschema
    except ModuleNotFoundError as error:
        if error.name != 'jsonschema':
            raise
        raise RuntimeError('missing python3 module jsonschema; '
                           'install python3-jsonschema or pip install jsonschema') from None
    return jsonschema


def bundle(path=None):
    jsonschema = require_jsonschema()
    p = (Path(path) if path else HOME / 'current').resolve(strict=True)
    require(p.parent == HOME / 'releases' and re.fullmatch(r'[0-9a-f]{40}', p.name),
            'bundle must be /opt/signet/releases/<full source commit>')
    # Site INI is writable by the daemon. Executable bundles must not be.
    for q in [p, *p.rglob('*')]:
        s = q.lstat()
        require(not q.is_symlink() and s.st_uid == 0 and not s.st_mode & 0o022,
                f'untrusted release bundle path: {q}')
    m = json.loads((p / 'manifest.json').read_text())
    jsonschema.Draft202012Validator(json.loads((p / 'signet/deploy/manifest.schema.json').read_text()),
                                   format_checker=jsonschema.FormatChecker()).validate(m)
    require(m['schema_version'] == 1 and m['source_commit'] == p.name, 'manifest identity mismatch')
    require(set(m['profiles']) <= PROFILES and len(set(m['profiles'])) == len(m['profiles']), 'invalid profiles')
    require('ctap' not in m['profiles'] or 'passkeys' in m['profiles'], 'CTAP requires passkeys')
    e = env_file(p / 'release.env', {'SIGNET_IMAGE', 'CADDY_IMAGE'})
    variant = 'passkeys' if 'passkeys' in m['profiles'] else 'default'
    require(DIGEST.fullmatch(e['SIGNET_IMAGE']) and e['SIGNET_IMAGE'] == m['images'][variant], 'image differs from manifest')
    require(all(DIGEST.fullmatch(m['images'][v]) for v in ('default', 'passkeys')), 'unpinned variant')
    require(DIGEST.fullmatch(e['CADDY_IMAGE']) and e['CADDY_IMAGE'] == m['images']['caddy'], 'Caddy differs from manifest')
    return p, m, e


def site():
    return env_file(HOME / 'config/deploy.env',
                    {'SIGNET_HEALTH_PORT', 'SIGNET_ENROLLMENT_HOST', 'SIGNET_UHID_GID',
                     'SPACES_ENDPOINT', 'SPACES_BUCKET', 'SPACES_PREFIX'})


def compose(p, m, *args, capture=False):
    cmd = ['docker', 'compose', '--project-name', 'signet',
           '--env-file', HOME / 'config/deploy.env', '--env-file', p / 'release.env',
           '-f', p / 'signet/docker-compose.yml']
    for profile in m['profiles']:
        cmd += ['-f', p / f'signet/deploy/compose.{profile}.yml']
        if profile == 'bootstrap':
            cmd += ['--profile', 'bootstrap']
    cmd += ['--profile', 'ops', *args]
    return run(cmd, capture=capture)


def store(p, m, *args):
    return compose(p, m, 'run', '--rm', '--no-deps', '-T', 'signet-ops', *args)


def inspect_store(p, m, kind='state', path=None):
    result = compose(p, m, 'run', '--rm', '--no-deps', '-T', '--entrypoint',
                     'signet-inspect-store', 'signet-ops', kind,
                     *([path] if path else []), capture=True).stdout.splitlines()
    # Some SQLCipher versions print "ok" for PRAGMA key as well.
    require(result and result[0] == 'ok', 'SQLCipher integrity check failed')
    while result and result[0] == 'ok':
        result.pop(0)
    require(result and result[0].isdigit() and len(result) > 1, 'SQLCipher integrity/schema inspection failed')
    return {'schema_version': int(result[0]),
            'schema_sha256': hashlib.sha256('\n'.join(result[1:]).encode()).hexdigest()}


def storage(m):
    actual = json.loads(run(['findmnt', '--json', '--mountpoint', VOLUME, '-o', 'TARGET,SOURCE,UUID,FSTYPE'], capture=True).stdout)['filesystems']
    require(len(actual) == 1 and actual[0]['target'] == str(VOLUME), 'volume is not mounted')
    a = actual[0]
    require(a['uuid'] == m['volume']['filesystem_uuid'] and a['fstype'] == 'ext4', 'wrong volume UUID/type')
    dev = Path('/dev/disk/by-id') / ('scsi-0DO_Volume_' + m['volume']['name'])
    require(dev.resolve(strict=True) == Path(a['source']).resolve(strict=True), 'wrong DO volume device')
    owned(Path('/etc/signet-volume-id'), 0, 0o600)
    require(Path('/etc/signet-volume-id').read_text().strip() == m['volume']['id'], 'wrong provisioned volume ID')


def wait_rule(wait=False):
    active = VOLUME / 'active.json'
    if active.exists():
        prior = json.loads(active.read_text())
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        require(prior['boot_id'] != boot, 'previous daemon has no stop receipt on this boot; investigate before restart')
        elapsed = float(Path('/proc/uptime').read_text().split()[0])
        if elapsed < 270 and wait:
            time.sleep(270 - elapsed)
            return wait_rule(False)
        require(elapsed >= 270, 'unclean reboot: wait at least 270 seconds after boot')
        return
    if not STOP.exists():
        require(not (VOLUME / 'state/signet.db').exists(), 'existing store has no stop receipt; owner must establish a 270s stopped interval')
        return
    receipt = json.loads(STOP.read_text())
    # Wall-clock jumps backwards fail closed; monotonic only applies within a boot.
    elapsed = time.time() - receipt['wall_time']
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if receipt['boot_id'] == boot:
        elapsed = min(elapsed, time.monotonic() - receipt['monotonic'])
    require(elapsed >= 0, 'clock moved backwards since stop')
    if elapsed < 270 and wait:
        time.sleep(270 - elapsed)
        return wait_rule(False)
    require(elapsed >= 270, f'restart refused: {270 - elapsed:.0f}s remain in stop quarantine')


def image_check(p, m, e, running=False):
    if 'bootstrap' in m['profiles']:
        caddy = json.loads(run(['docker', 'image', 'inspect', e['CADDY_IMAGE']], capture=True).stdout)[0]
        require(e['CADDY_IMAGE'] in caddy['RepoDigests'], 'local Caddy digest mismatch')
    expected = json.loads(run(['docker', 'image', 'inspect', e['SIGNET_IMAGE']], capture=True).stdout)[0]
    require(e['SIGNET_IMAGE'] in expected['RepoDigests'], 'local image digest mismatch')
    if running:
        cid = compose(p, m, 'ps', '-q', 'signetd', capture=True).stdout.strip()
        require(cid, 'daemon absent')
        c = json.loads(run(['docker', 'inspect', cid], capture=True).stdout)[0]
        require(c['State']['Running'] and c['Image'] == expected['Id'] and
                c['Config']['Image'] == e['SIGNET_IMAGE'], 'running image differs from manifest')


def preflight(path=None, starting=True, images=True):
    p, m, e = bundle(path)
    storage(m)  # Do this BEFORE touching any state directory.
    owned(HOME / 'config', 10001, 0o750, True)
    owned(HOME / 'config/signet.conf', 10001, 0o640)
    owned(HOME / 'config/deploy.env', 10001, 0o640)
    owned(HOME / 'secrets', 0, 0o700, True)
    for name in ('db.key', 'bunker.nsec', 'backup.key'):
        owned(HOME / 'secrets' / name, 10001, 0o400)
        require((HOME / 'secrets' / name).stat().st_size > 0, f'empty secret file: {name}')
    for name in ('state', 'backups'):
        owned(VOLUME / name, 10001, 0o700, True)
    run(['setpriv', '--reuid=10001', '--regid=10001', '--clear-groups',
         'test', '-w', HOME / 'config'])
    cfg = configparser.ConfigParser(interpolation=None)
    with (HOME / 'config/signet.conf').open() as f:
        cfg.read_file(f)
    for section in cfg.sections():
        for name in ('sync_key', 'bunker_nsec', 'master_key', 'db_key', 'backup_key', 'provisioner_nsec'):
            require(not cfg.get(section, name, fallback='').strip(), 'secret value in non-secret config; use mounted files')
    require(0 < cfg.getint('replay', 'skew_seconds', fallback=120) <= 120,
            'production replay skew must be bounded to 1..120 seconds')
    require(cfg.getint('replay', 'max_entries', fallback=50000) > 0,
            'production replay cache cannot be disabled')
    s = site()
    require(cfg.get('store', 'db_path') == '/data/signet.db', 'unsupported DB path')
    require(cfg.get('policy_defaults', 'policy_file') == '/data/policies.conf', 'policy must be durable')
    require(cfg.get('audit', 'path') == '/data/audit.jsonl', 'audit must be durable')
    require(cfg.getint('server', 'health_port') == int(s['SIGNET_HEALTH_PORT']) and
            0 < int(s['SIGNET_HEALTH_PORT']) < 65536, 'health port mismatch/disabled')
    require(re.fullmatch('[0-9a-f]{64}', cfg.get('nostr', 'bunker_pubkey')) and
            cfg.get('nostr', 'bunker_pubkey') == m['public_identifiers']['bunker'], 'public bunker identity mismatch')
    require(cfg.getint('bootstrap', 'port', fallback=0) == (9487 if 'bootstrap' in m['profiles'] else 0), 'bootstrap profile/config mismatch')
    for section, key in [('dbus', 'unix_enabled'), ('dbus', 'tcp_enabled'), ('nip5l', 'enabled'), ('ssh_agent', 'enabled')]:
        require(not cfg.getboolean(section, key, fallback=False), f'unsupported transport: {section}.{key}')
    if 'passkeys' in m['profiles']:
        owned(HOME / 'secrets/passkey-sync.key', 10001, 0o400)
    else:
        require(not cfg.getboolean('passkeys', 'enabled', fallback=False), 'passkeys needs passkeys image')
    if 'ctap' in m['profiles']:
        require(stat.S_ISCHR(Path('/dev/uhid').stat().st_mode), 'UHID unavailable')
    require(cfg.getboolean('passkeys', 'virtual_ctap', fallback=False) == ('ctap' in m['profiles']), 'CTAP profile/config mismatch')
    if starting:
        wait_rule()
    if images:
        image_check(p, m, e)
    compose(p, m, 'config', '--quiet')
    return p, m, e


@contextlib.contextmanager
def maintenance():
    with open('/run/lock/signet-maintenance.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def backup(p, m, upload=True):
    name = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + os.urandom(4).hex()
    target = VOLUME / 'backups' / name
    target.mkdir(mode=0o700)
    os.chown(target, 10001, 10001)
    store(p, m, 'backup-db', f'/backups/{name}/store.db')
    inspect_store(p, m, 'backup', f'/backups/{name}/store.db')
    files = {'signet.conf': HOME / 'config/signet.conf', 'deploy.env': HOME / 'config/deploy.env',
             'policies.conf': VOLUME / 'state/policies.conf', 'audit.jsonl': VOLUME / 'state/audit.jsonl',
             'manifest.json': p / 'manifest.json'}
    # Fixed allowlist: keys, URI handoffs and credentials are never archived.
    with tarfile.open(target / 'config.tar.gz', 'w:gz') as out:
        for dest, src in files.items():
            require(src.is_file() and not src.is_symlink(), f'missing/unsafe backup input: {src}')
            out.add(src, arcname=dest, recursive=False)
    sums = {f.name: hashlib.file_digest(f.open('rb'), 'sha256').hexdigest()
            for f in (target / 'store.db', target / 'config.tar.gz')}
    atomic_json(target / 'checksums.json', sums)
    reference = str(target)
    if upload:
        s = site()
        require(re.fullmatch(r'https://[a-z0-9-]+\.digitaloceanspaces\.com', s['SPACES_ENDPOINT']), 'invalid Spaces endpoint')
        require(re.fullmatch(r'[a-z0-9][a-z0-9.-]+', s['SPACES_BUCKET']), 'invalid Spaces bucket')
        owned(HOME / 'secrets/spaces.credentials', 0, 0o400)
        reference = f"s3://{s['SPACES_BUCKET']}/{s.get('SPACES_PREFIX', 'signet')}/{name}/"
        for f in ('store.db', 'config.tar.gz', 'checksums.json'):
            run(['env', 'AWS_SHARED_CREDENTIALS_FILE=/opt/signet/secrets/spaces.credentials',
                 'AWS_EC2_METADATA_DISABLED=true', 'aws', '--endpoint-url', s['SPACES_ENDPOINT'],
                 's3', 'cp', target / f, reference + f, '--only-show-errors'])
    m['last_successful_backup'] = {'reference': reference, 'local_path': str(target), 'checksums': sums, 'off_host': upload}
    atomic_json(p / 'manifest.json', m)
    return str(target)


def health(p, m, e):
    image_check(p, m, e, running=True)
    for route in ('health', 'ready'):
        with urllib.request.urlopen(f"http://127.0.0.1:{site()['SIGNET_HEALTH_PORT']}/{route}", timeout=10) as response:
            require(response.status == 200, f'{route} failed')


def switch(p):
    link = HOME / 'current.next'
    require(not link.exists() and not link.is_symlink(), 'stale current.next; inspect before retry')
    link.symlink_to(p)
    os.replace(link, HOME / 'current')


def confirmed_stopped(p, m):
    services = ['signetd'] + (['caddy'] if 'bootstrap' in m['profiles'] else [])
    ids = compose(p, m, 'ps', '--all', '-q', *services, capture=True).stdout.split()
    if ids:
        states = json.loads(run(['docker', 'inspect', *ids], capture=True).stdout)
        require(all(not c['State']['Running'] and not c['State']['Restarting'] for c in states),
                'supervised container has not stopped')


def stop(p, m):
    run(['systemctl', 'stop', 'signet-compose.service'])
    require(not compose(p, m, 'ps', '-q', 'signetd', capture=True).stdout.strip(), 'daemon still present')
    require(STOP.exists(), 'missing durable stop receipt')


def release(args):
    p, m, e = preflight(starting=False)
    q, n, f = bundle(args.bundle)
    require(q != p, 'candidate is already current')
    require(n['volume'] == m['volume'], 'candidate changes volume identity')
    if args.action == 'upgrade':
        # Failure to pull leaves the current daemon and release untouched.
        if args.offline:
            print('Offline upgrade: validating preloaded pinned images; no registry pull.', flush=True)
        else:
            compose(q, n, 'pull', 'signetd', *(['caddy'] if 'bootstrap' in n['profiles'] else []))
        preflight(q, starting=False)  # Still verifies every selected local digest before stop.
        stop(p, m)
        before = inspect_store(p, m)
        saved = backup(p, m, upload=False)
        n['upgrade'] = {'previous': str(p), 'backup': saved, 'before': before, 'after': None}
        atomic_json(q / 'manifest.json', n)
    else:
        u = m['upgrade']
        require(u['previous'] == str(q), 'rollback target is not recorded predecessor')
        if args.action == 'rollback-image':
            require(u['after'] is not None and u['before'] == u['after'] == inspect_store(p, m),
                    'schema changed or post-upgrade inspection missing: image rollback refused')
            stop(p, m)
        else:
            require(args.confirm == 'RESTORE-REVOKED-AUTHORITY', 'state rollback requires explicit authorization-loss confirmation')
            stop(p, m)
            # Preserve candidate state/config before replacing anything.
            recovery = VOLUME / 'backups' / ('pre-rollback-' + str(time.time_ns()))
            recovery.mkdir(mode=0o700)
            shutil.copytree(VOLUME / 'state', recovery / 'state')
            shutil.copytree(HOME / 'config', recovery / 'config')
            saved = Path(u['backup'])
            require(saved.parent == VOLUME / 'backups' and not saved.is_symlink(), 'unsafe recovery path')
            sums = json.loads((saved / 'checksums.json').read_text())
            for filename in ('store.db', 'config.tar.gz'):
                with (saved / filename).open('rb') as stream:
                    require(hashlib.file_digest(stream, 'sha256').hexdigest() == sums[filename], 'backup checksum mismatch')
            for suffix in ('.pre-restore', '.restoring'):
                old = VOLUME / 'state' / ('signet.db' + suffix)
                if old.exists():
                    shutil.move(old, recovery / old.name)
            store(q, n, 'restore-db', f'/backups/{saved.name}/store.db', '--confirm')
            # Never extract arbitrary tar paths or restore the old host manifest.
            with tarfile.open(saved / 'config.tar.gz') as archive:
                for name, dest in [('signet.conf', HOME / 'config/signet.conf'), ('deploy.env', HOME / 'config/deploy.env'),
                                   ('policies.conf', VOLUME / 'state/policies.conf'), ('audit.jsonl', VOLUME / 'state/audit.jsonl')]:
                    member = archive.getmember(name)
                    require(member.isfile(), 'unsafe recovery member')
                    with archive.extractfile(member) as src, dest.open('wb') as out:
                        shutil.copyfileobj(src, out)
                    os.chown(dest, 10001, 10001)
                    os.chmod(dest, 0o640 if name in ('signet.conf', 'deploy.env') else 0o600)
            store(q, n, 'verify-audit')
            store(q, n, 'list-agents', '--verify')
    switch(q)
    wait_rule(wait=True)
    preflight(q)
    run(['systemctl', 'start', 'signet-compose.service'])
    # Daemon startup is not relay catch-up. Wait on readiness with a bounded
    # health probe, never restart/re-pair just because a relay is unavailable.
    deadline = time.monotonic() + 180
    while True:
        try:
            health(q, n, f)
            break
        except Exception:
            if time.monotonic() >= deadline:
                raise
            time.sleep(2)
    if args.action == 'upgrade':
        n['upgrade']['after'] = inspect_store(q, n)
        atomic_json(q / 'manifest.json', n)
    print('Host checks passed. Run signed management + existing-client proof; no automatic rollback.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['preflight', 'backup', 'upgrade', 'rollback-image', 'rollback-state', 'supervise', 'stop', 'mark-stop', 'quarantine', 'health'])
    parser.add_argument('bundle', nargs='?')
    parser.add_argument('--confirm')
    parser.add_argument('--offline', action='store_true',
                        help='upgrade only: require preloaded pinned images instead of pulling')
    args = parser.parse_args()
    require(not args.offline or args.action == 'upgrade', '--offline is only valid for upgrade')
    require(os.geteuid() == 0, 'host tooling requires root')
    if args.action == 'preflight':
        preflight(args.bundle)
    elif args.action == 'supervise':
        p, m, _ = preflight()
        # Attached up makes systemd observe daemon failure. Compose never restarts it.
        atomic_json(VOLUME / 'active.json', {'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()})
        Path('/run/signet-started').touch(mode=0o600)
        services = ['signetd'] + (['caddy'] if 'bootstrap' in m['profiles'] else [])
        compose(p, m, 'up', '--no-build', '--pull', 'never', '--abort-on-container-exit', '--exit-code-from', 'signetd', *services)
        raise RuntimeError('supervised container exited; systemd will enforce restart quarantine')
    elif args.action == 'stop':
        p, m, _ = bundle()
        compose(p, m, 'down', '--timeout', '30')
    elif args.action == 'quarantine':
        with maintenance():
            p, m, _ = bundle()
            storage(m)
            require(run(['systemctl', 'show', '-p', 'ActiveState', '--value', 'signet-compose.service'], capture=True).stdout.strip() in ('inactive', 'failed'), 'service is not stopped')
            confirmed_stopped(p, m)
            atomic_json(STOP, {'wall_time': time.time(), 'monotonic': time.monotonic(),
                              'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                              'service_result': 'operator-established-stopped-interval'})
            (VOLUME / 'active.json').unlink(missing_ok=True)
    elif args.action == 'mark-stop':
        p, m, _ = bundle()
        storage(m)
        # ExecStopPost can run after preflight refusal; do not extend quarantine
        # unless ExecStart actually ran (systemd supplies INVOCATION_ID marker).
        marker = Path('/run/signet-started')
        if marker.exists():
            # Failure to query Docker is an uncertain stop, never a safe receipt.
            confirmed_stopped(p, m)
            atomic_json(STOP, {'wall_time': time.time(), 'monotonic': time.monotonic(),
                              'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                              'service_result': os.getenv('SERVICE_RESULT', 'unknown')})
            (VOLUME / 'active.json').unlink(missing_ok=True)
            marker.unlink()
    elif args.action == 'health':
        health(*bundle())
    else:
        with maintenance():
            if args.action == 'backup':
                p, m, e = preflight(starting=False)
                image_check(p, m, e, running=True)
                backup(p, m)
            else:
                release(args)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        print(f'signet deployment refused: {error}', file=sys.stderr)
        sys.exit(1)
