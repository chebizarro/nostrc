"""Network namespaces, veth pairs, bridges, nftables and netem for one lab run.

Everything a run creates is a namespace named `<tag>-<name>`; deleting the
namespaces deletes their veths, bridges, addresses, qdiscs and nft tables with
them, so teardown is one idempotent sweep (`Lab.teardown`, also usable on a
stale prefix from a crashed run via `Lab.purge`). Processes still inside a
namespace are killed first so the namespace can go.
"""
import re
import subprocess
import time


class LabError(Exception):
    pass


def run(argv, *, input=None, check=True, timeout=60):
    try:
        result = subprocess.run(argv, input=input, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        raise LabError(f'{" ".join(argv)}: timed out after {timeout}s') from e
    if check and result.returncode != 0:
        raise LabError(f'{" ".join(argv)}: exit {result.returncode}: {result.stderr.strip() or result.stdout.strip()}')
    return result


class Netns:
    def __init__(self, lab, name):
        self.lab = lab
        self.name = name

    def exec_argv(self, argv):
        return ['ip', 'netns', 'exec', self.name, *argv]

    def sh(self, *argv, **kw):
        return run(self.exec_argv(list(argv)), **kw)

    def ip(self, *argv, **kw):
        return run(['ip', '-n', self.name, *argv], **kw)

    def link_up(self, dev):
        self.ip('link', 'set', 'dev', dev, 'up')

    def link_down(self, dev):
        self.ip('link', 'set', 'dev', dev, 'down')

    def link_mtu(self, dev, mtu):
        self.ip('link', 'set', 'dev', dev, 'mtu', str(mtu))

    def link_stat(self, dev, field):
        out = self.ip('-s', '-j', 'link', 'show', 'dev', dev).stdout
        import json
        row = json.loads(out)[0]
        if field.startswith('rx_'):
            return row['stats64']['rx'][field[3:]]
        return row['stats64']['tx'][field[3:]]

    def netem(self, dev, spec):
        """Replace the root qdisc of `dev` with netem `spec`, or remove it when spec is None."""
        if spec is None:
            self.sh('tc', 'qdisc', 'del', 'dev', dev, 'root', check=False)
        else:
            self.sh('tc', 'qdisc', 'replace', 'dev', dev, 'root', 'netem', *spec.split())

    def sysctl(self, key, value):
        self.sh('sysctl', '-q', '-w', f'{key}={value}')

    def nft(self, ruleset):
        self.sh('nft', '-f', '-', input=ruleset)

    def frag_creates(self):
        """IPv4 fragments this namespace's stack created (`Ip: FragCreates` in /proc/net/snmp)."""
        text = self.sh('cat', '/proc/net/snmp').stdout
        rows = [line.split() for line in text.splitlines() if line.startswith('Ip:')]
        if len(rows) < 2:
            raise LabError(f'{self.name}: /proc/net/snmp has no Ip rows')
        return int(dict(zip(rows[0][1:], rows[1][1:]))['FragCreates'])

    def conntrack_udp(self):
        result = self.sh('conntrack', '-L', '-p', 'udp', check=False)
        return result.stdout + result.stderr

    def pids(self):
        out = run(['ip', 'netns', 'pids', self.name], check=False).stdout.split()
        return [int(p) for p in out if p.isdigit()]

    def diagnostics(self):
        parts = [f'===== netns {self.name} =====']
        for title, argv in (('addresses', ['ip', '-br', 'addr']), ('links', ['ip', '-s', 'link']),
                            ('routes', ['ip', 'route']), ('qdiscs', ['tc', 'qdisc', 'show']),
                            ('nft', ['nft', 'list', 'ruleset']), ('conntrack udp', ['conntrack', '-L', '-p', 'udp']),
                            ('ip snmp', ['grep', '^Ip:', '/proc/net/snmp']),
                            ('udp sockets', ['ss', '-uanp'])):
            result = self.sh(*argv, check=False)
            parts.append(f'--- {title} ---\n{result.stdout}{result.stderr}'.rstrip())
        return '\n'.join(parts) + '\n'


class Lab:
    PREFIX = 'fc'

    def __init__(self, tag):
        if not re.fullmatch(r'[a-z0-9]{1,9}', tag):
            raise LabError(f'lab tag {tag!r} must be 1-9 lowercase alphanumerics (interface names are bounded)')
        self.tag = f'{self.PREFIX}{tag}'
        self.namespaces = []
        self.counter = 0

    @staticmethod
    def existing(prefix=PREFIX):
        names = []
        for line in run(['ip', 'netns', 'list'], check=False).stdout.splitlines():
            name = line.split()[0] if line.split() else ''
            if name.startswith(prefix):
                names.append(name)
        return names

    @staticmethod
    def delete_netns(name):
        for pid in [int(p) for p in run(['ip', 'netns', 'pids', name], check=False).stdout.split() if p.isdigit()]:
            run(['kill', '-9', str(pid)], check=False)
        for _ in range(20):
            if run(['ip', 'netns', 'del', name], check=False).returncode == 0:
                return True
            time.sleep(0.1)
        return False

    @classmethod
    def purge(cls, prefix=PREFIX):
        """Delete every namespace left behind under `prefix`; returns the names removed."""
        removed = []
        for name in cls.existing(prefix):
            if cls.delete_netns(name):
                removed.append(name)
        return removed

    def netns(self, short):
        name = f'{self.tag}-{short}'
        if name in self.existing(name):
            self.delete_netns(name)
        run(['ip', 'netns', 'add', name])
        ns = Netns(self, name)
        self.namespaces.append(ns)
        ns.ip('link', 'set', 'lo', 'up')
        return ns

    def veth(self, a, a_dev, a_cidr, b, b_dev, b_cidr):
        """A veth pair with one end in each namespace, addressed and up."""
        self.counter += 1
        ta, tb = f'{self.tag}{self.counter}a', f'{self.tag}{self.counter}b'
        run(['ip', 'link', 'add', ta, 'type', 'veth', 'peer', 'name', tb])
        try:
            run(['ip', 'link', 'set', ta, 'netns', a.name])
            run(['ip', 'link', 'set', tb, 'netns', b.name])
        except LabError:
            run(['ip', 'link', 'del', ta], check=False)
            raise
        for ns, tmp, dev, cidr in ((a, ta, a_dev, a_cidr), (b, tb, b_dev, b_cidr)):
            ns.ip('link', 'set', tmp, 'name', dev)
            if cidr:
                ns.ip('addr', 'add', cidr, 'dev', dev)
            ns.link_up(dev)

    def bridge(self, ns, name, cidr=None):
        ns.ip('link', 'add', name, 'type', 'bridge')
        if cidr:
            ns.ip('addr', 'add', cidr, 'dev', name)
        ns.link_up(name)

    def enslave(self, ns, dev, bridge):
        ns.ip('link', 'set', dev, 'master', bridge)

    def diagnostics(self):
        return ''.join(ns.diagnostics() for ns in self.namespaces)

    def teardown(self):
        failed = []
        for ns in reversed(self.namespaces):
            if not self.delete_netns(ns.name):
                failed.append(ns.name)
        self.namespaces = []
        return failed
