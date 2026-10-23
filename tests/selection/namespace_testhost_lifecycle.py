"""Exact-token disposable testhost ownership, including create/reply-loss cleanup."""
import json
from pathlib import Path
import re
import subprocess
import sys
import uuid

LABEL = 'nostrc.namespace-policy'


def docker(*args):
    result = subprocess.run(['docker', *args], capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError(f'docker {args[0]} failed: {result.stderr}')
    return result.stdout.strip()


def candidates(token):
    output = docker('container', 'ls', '--all', '--no-trunc', '--filter', f'label={LABEL}={token}',
                    '--format', '{{.ID}}')
    ids = output.splitlines() if output else []
    if len(ids) > 1 or any(not re.fullmatch(r'[0-9a-f]{64}', cid) for cid in ids):
        raise RuntimeError('ambiguous exact-token container ownership')
    return ids


def prepare(control):
    image = (control / 'image.id').read_text().strip()
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', image):
        raise ValueError('invalid immutable image ID')
    token = uuid.uuid4().hex
    state = dict(token=token, name='nostrc-namespace-policy-' + token, image=image)
    # Persist BEFORE docker create; the container never mounts this control path.
    (control / 'owner.json').write_text(json.dumps(state) + '\n')
    (control / 'owner.token').write_text(token + '\n')
    (control / 'container.name').write_text(state['name'] + '\n')


def cleanup(control):
    result = dict(kind='namespace_testhost_cleanup_v1', cleanup_complete=False,
                  scope='owned_disposable_docker_container_only', complete=False)
    try:
        state = json.loads((control / 'owner.json').read_text())
        token, name, image = (state[key] for key in ('token', 'name', 'image'))
        if (not re.fullmatch(r'[0-9a-f]{32}', token)
                or name != 'nostrc-namespace-policy-' + token
                or not re.fullmatch(r'sha256:[0-9a-f]{64}', image)):
            raise ValueError('invalid persisted testhost ownership')
        ids = candidates(token)  # Failed daemon queries never prove absence.
        recorded = (control / 'container.cid').read_text().strip() if (control / 'container.cid').exists() else ''
        if ids:
            cid = ids[0]
            if re.fullmatch(r'[0-9a-f]{64}', recorded) and recorded != cid:
                raise ValueError('recorded CID differs from exact-token container')
            observed = json.loads(docker('container', 'inspect', cid))[0]
            result['observed'] = observed
            if (observed['Id'] != cid or observed['Name'] != '/' + name or observed['Image'] != image
                    or observed['Config']['Labels'].get(LABEL) != token
                    or observed['Config']['User'] != '1001:1001'
                    or observed['HostConfig']['Privileged']
                    or observed['HostConfig']['CapAdd']
                    or observed['HostConfig']['NetworkMode'] != 'none'
                    or not observed['HostConfig']['ReadonlyRootfs']):
                raise ValueError('testhost identity/setup differs; removal refused')
            result['removed_cid'] = cid
            result['removal_stdout'] = docker('container', 'rm', '--force', '--volumes', cid)
        if candidates(token):
            raise RuntimeError('owned testhost remains after removal')
        result['cleanup_complete'] = True
    except Exception as exc:
        result['error'] = str(exc)
    (control / 'cleanup.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['cleanup_complete'] else 1


if __name__ == '__main__':
    operation, directory = sys.argv[1:]
    if operation == 'prepare':
        prepare(Path(directory))
    elif operation == 'cleanup':
        sys.exit(cleanup(Path(directory)))
    else:
        raise SystemExit('expected prepare or cleanup')
