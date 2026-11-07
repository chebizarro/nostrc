#!/bin/sh
set -eu
exec python3 - "$@" <<'PY'
import os, pathlib, shutil, socket, sqlite3, subprocess, sys, tempfile

daemon, ctl = map(os.path.abspath, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='signet-startup-') as tmp:
    root = pathlib.Path(tmp)
    env = {k:v for k,v in os.environ.items() if not k.startswith('SIGNET_')}
    bunker = root/'bunker'; bunker.write_text('01'*32+'\n'); bunker.chmod(0o600)
    master = root/'master'; master.write_text('0123456789abcdef'*4+'\n'); master.chmod(0o400)
    env.update(SIGNET_BUNKER_NSEC_FILE=str(bunker), SIGNET_DB_KEY_FILE=str(master),
               SIGNET_REQUIRE_ENCRYPTED_DB='true')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
    cfg = root/'signet.conf'
    db = root/'signet.db'
    def config(path=db):
        cfg.write_text(f'[server]\nhealth_port={port}\n[store]\ndb_path={path}\n'
                       '[nostr]\nrelays=wss://127.0.0.1:1\n[audit]\nstdout=false\n')
    config()
    def refused(name, args=None, overrides=None):
        e = env.copy(); e.update(overrides or {})
        p = subprocess.run([daemon, '-c', str(cfg)] if args is None else [daemon]+args,
                           env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        assert p.returncode != 0, (name, 'unexpected success')
        assert p.returncode > 0, (name, 'crashed', p.stdout.decode(errors='replace'))
        with socket.socket() as s:
            s.settimeout(1)
            assert s.connect_ex(('127.0.0.1', port)) != 0, (name, 'HTTP listener exposed')
        if shutil.which('ss'):
            listing = subprocess.check_output(['ss', '-lnt'], text=True)
            assert not any(line.split()[3].endswith(':'+str(port)) for line in listing.splitlines()[1:]), name
        print(name+': PASS')
        return p.stdout
    refused('missing explicit config', ['-c', str(root/'missing')])
    cfg.write_text('[invalid\nsecret=do-not-print-this\n')
    assert b'do-not-print-this' not in refused('malformed explicit config')
    config()
    refused('empty master key', overrides={'SIGNET_DB_KEY_FILE':'', 'SIGNET_DB_KEY':''})
    refused('unavailable master key file', overrides={'SIGNET_DB_KEY_FILE':str(root/'absent')})
    refused('ambiguous master key', overrides={'SIGNET_DB_KEY':'value-must-not-be-logged'})
    config('')
    refused('empty database path')
    config(root/'missing-parent'/'state.db')
    refused('unavailable state directory')
    blocked = root/'blocked'; blocked.mkdir(); blocked.chmod(0o500)
    if os.geteuid() != 0:
        config(blocked/'state.db'); refused('unwritable state directory')
    else:
        print('CAPABILITY: root bypasses directory DAC; unavailable-directory refusal exercised')
    blocked.chmod(0o700)
    config()
    with sqlite3.connect(db) as con: con.execute('CREATE TABLE fixture(value TEXT)')
    migrated = subprocess.run([ctl, '-c', str(cfg), 'migrate-db'], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    if migrated.returncode == 0:
        wrong = root/'wrong'; wrong.write_text('fedcba9876543210'*4); wrong.chmod(0o400)
        refused('wrong database key', overrides={'SIGNET_DB_KEY_FILE':str(wrong)})
    else:
        assert b'not linked against SQLCipher' in migrated.stdout, migrated.stdout
        refused('plain SQLite build refuses required encryption')
        print('CAPABILITY: wrong-key case requires SQLCipher; covered by SQLCipher suite')
    # Failure of a later enabled listener unwinds an already-started bootstrap
    # listener before freeing its borrowed stores.
    with socket.socket() as busy, socket.socket() as probe:
        busy.bind(('127.0.0.1', port)); busy.listen()
        probe.bind(('127.0.0.1', 0)); bootstrap_port = probe.getsockname()[1]
        probe.close()
        cfg.write_text(cfg.read_text()+f'\n[bootstrap]\nport={bootstrap_port}\n')
        e = env.copy(); e['SIGNET_REQUIRE_ENCRYPTED_DB'] = 'false'
        p = subprocess.run([daemon, '-c', str(cfg)], env=e, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=15)
        assert p.returncode > 0, ('occupied health port', p.returncode, p.stdout)
        assert b'failed to start health server' in p.stdout, p.stdout
        assert b'entering event-driven main loop' not in p.stdout
        with socket.socket() as s:
            s.settimeout(1)
            assert s.connect_ex(('127.0.0.1', bootstrap_port)) != 0, 'bootstrap survived unwind'
        print('enabled listener failure and bootstrap unwind: PASS')
print('daemon startup tests: PASS')
PY
