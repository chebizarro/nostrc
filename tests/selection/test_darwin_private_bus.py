"""Darwin custody private session bus: real native runs (C helper, Homebrew dbus).

Diagnostic resource proof, never .23/profile acceptance. The bus runs as a
custody member from the journaled owned directory; the outer adapter releases
that directory only after a validated helper exit.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'scripts'
HELPER = SCRIPTS / 'adapter_darwin_custody.py'
sys.path.insert(0, str(SCRIPTS))

TARGET = r'''
import json, os, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, SCRIPTS)
from test_selection_services import darwin_private_bus
root, evidence, mode, report = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
if mode == 'hungdaemon':
    # The real daemon inherits a blocked SIGTERM, so graceful shutdown hangs.
    signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})
with darwin_private_bus(evidence, root) as state:
    out = {'address': os.environ['DBUS_SESSION_BUS_ADDRESS'], 'home': os.environ['HOME'],
           'launchd': os.environ.get('DBUS_LAUNCHD_SESSION_BUS_SOCKET'), 'daemon_pid': state['services'][0]['pid'],
           'bus_id': state['bus_id'], 'supervisor': os.getppid()}
    names = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path',
                            '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.ListNames'],
                           capture_output=True, text=True, timeout=15)
    out['gdbus'] = [names.returncode, names.stdout]
    if mode == 'env':
        out['dbus_env'] = {k: v for k, v in os.environ.items() if k.startswith('DBUS_')}
        child = subprocess.run([sys.executable, '-c', 'import json, os; print(json.dumps(sorted(k for k in os.environ if k.startswith("DBUS_"))))'],
                               capture_output=True, text=True)
        out['consumer_dbus_keys'] = json.loads(child.stdout)
    if mode == 'asan':
        consumer = subprocess.run([sys.argv[5]], capture_output=True, text=True, timeout=30)
        out['asan'] = [consumer.returncode, consumer.stdout, consumer.stderr[-2000:]]
    if mode in ('escape', 'hold'):
        if os.fork() == 0:
            os.setsid()
            if os.fork() == 0:
                signal.signal(signal.SIGTERM, signal.SIG_IGN)  # survives exec
                os.execvp('gdbus', ['gdbus', 'monitor', '--session', '--dest', 'org.freedesktop.DBus'])
            os._exit(0)
        os.wait()
        time.sleep(0.5)
        monitors = subprocess.run(['pgrep', '-f', 'gdbus monitor --session --dest org.freedesktop.DBus'],
                                  capture_output=True, text=True).stdout.split()
        out['escaped'] = [int(p) for p in monitors]
    report.write_text(json.dumps(out))
    if mode == 'hold':
        time.sleep(120)
'''.replace('SCRIPTS', repr(str(SCRIPTS)))

CONSUMER = r'''
#include <gio/gio.h>
int main(void) {
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) { g_printerr("bus: %s\n", error->message); return 2; }
  GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetId", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
  if (!reply) { g_printerr("call: %s\n", error->message); return 3; }
  const gchar *id = NULL;
  g_variant_get(reply, "(&s)", &id);
  g_print("asan-consumer bus id %s\n", id);
  g_variant_unref(reply);
  g_object_unref(bus);
  return 0;
}
'''


def alive(pid):
    try:
        os.kill(pid, 0)  # observation only
        return True
    except ProcessLookupError:
        return False


@unittest.skipUnless(sys.platform == 'darwin', 'Darwin custody private bus is macOS-only')
class DarwinPrivateBus(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='nostrc-darwin-bus-')
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name).resolve()
        self.lane = self.work / 'lane'
        self.lane.mkdir()
        import test_selection_services as services
        self.services = services
        self.root = services.allocate_resource_mountpoint(self.lane, bases=services.DARWIN_RESOURCE_BASES)
        self.addCleanup(self.cleanup_root)

    def cleanup_root(self):
        if self.root.exists():  # only reached when a test intentionally retained it
            shutil.rmtree(self.root)

    def custody(self, mode, *extra, name='lane', env=None):
        """Run the target under real C custody, then the outer validated release."""
        from test_gate_darwin import darwin_command
        records, report = [], self.work / (name + '-report.json')
        argv = [sys.executable, '-c', TARGET, str(self.root), str(self.lane / 'services'), mode, str(report), *extra]
        try:
            darwin_command(argv, helper=HELPER, records=records, errors=[], log=self.lane / (name + '.log'), env=env)
            outcome = None
        except (RuntimeError, KeyboardInterrupt) as exc:
            outcome = exc
        record, = records
        if record['status'] in ('clean', 'recovered_lane_failed'):
            self.services.release_resource_directory(self.lane)
        else:
            self.services.retain_resource_directory(self.lane, 'test: custody exit not validated')
        report = json.loads(report.read_text()) if report.exists() else None
        return record, outcome, report

    def journal(self):
        return [r['state'] for r in self.services.read_resource_journal(self.lane / self.services.JOURNAL_NAME)]

    def test_real_bus_roundtrip_private_identity_and_owned_release(self):
        sibling = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.addCleanup(lambda: (sibling.kill(), sibling.wait()))
        record, outcome, report = self.custody('roundtrip')
        self.assertIsNone(outcome, record)
        self.assertEqual(record['status'], 'clean')
        self.assertTrue(report['address'].startswith('unix:path=' + str(self.root) + '/'))
        self.assertIsNone(report['launchd'])  # ambient launchd session bus stripped
        self.assertTrue(report['home'].startswith(str(self.root) + '/'))
        self.assertEqual(report['gdbus'][0], 0)
        self.assertIn("'org.freedesktop.DBus'", report['gdbus'][1])
        services = json.loads((self.lane / 'services/services.json').read_text())
        self.assertEqual((services['status'], services['bus_id']), ('passed', report['bus_id']))
        self.assertTrue(services['services'][0]['reaped'])
        self.assertFalse(alive(report['daemon_pid']))
        self.assertEqual(self.journal(), ['intent', 'allocated', 'released'])
        self.assertFalse(self.root.exists())
        self.assertIsNone(sibling.poll())

    def test_escaped_bus_client_is_owned_by_custody(self):
        record, outcome, report = self.custody('escape')
        self.assertIsNone(outcome, record)
        self.assertEqual(record['status'], 'clean')
        self.assertTrue(report['escaped'])
        self.assertGreaterEqual(record['control_final']['killed_count'], 1)
        for pid in report['escaped'] + [report['daemon_pid']]:
            self.assertFalse(alive(pid))
        self.assertFalse(self.root.exists())

    def test_no_inherited_dbus_route_survives(self):
        ambient = {'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent/nostrc-fake-system',
                   'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/nostrc-fake-session',
                   'DBUS_LAUNCHD_SESSION_BUS_SOCKET': '/nonexistent/nostrc-fake-launchd',
                   'DBUS_STARTER_ADDRESS': 'unix:path=/nonexistent/nostrc-fake-starter',
                   'DBUS_STARTER_BUS_TYPE': 'system',
                   'DBUS_ACTIVATION_ADDRESS': 'unix:path=/nonexistent/nostrc-fake-activation',
                   'DBUS_NOSTRC_FUTURE_ROUTE': 'unix:path=/nonexistent/nostrc-fake-future'}
        record, outcome, report = self.custody('env', name='ambient', env=dict(os.environ, **ambient))
        self.assertIsNone(outcome, record)
        self.assertEqual(set(report['dbus_env']), {'DBUS_SESSION_BUS_ADDRESS'})
        self.assertTrue(report['dbus_env']['DBUS_SESSION_BUS_ADDRESS'].startswith('unix:path=' + str(self.root) + '/'))
        self.assertEqual(report['consumer_dbus_keys'], ['DBUS_SESSION_BUS_ADDRESS'])  # a consumer process sees the same
        services = json.loads((self.lane / 'services/services.json').read_text())
        self.assertEqual(services['daemon_environment_dbus'], [])  # the daemon's exact exec environment
        for value in ambient.values():
            self.assertNotIn(value, json.dumps(report['dbus_env']))

    def test_hung_daemon_forced_kill_fails_the_lifecycle_and_keeps_proof(self):
        record, outcome, report = self.custody('hungdaemon', name='hung')
        self.assertIn('command exited', str(outcome))
        self.assertEqual(record['status'], 'clean')  # custody itself clean
        services = json.loads((self.lane / 'services/services.json').read_text())
        self.assertEqual(services['status'], 'failed')
        self.assertIn('forced SIGKILL', ' '.join(services['cleanup_errors']))
        daemon = services['services'][0]
        self.assertEqual((daemon['forced_exit'], daemon['reaped'], daemon['exit_code']), (True, True, -signal.SIGKILL))
        self.assertFalse(alive(report['daemon_pid']))
        self.assertIn(b'private bus cleanup unverified', (self.lane / 'hung.log').read_bytes())
        self.assertFalse(self.root.exists())

    def test_absent_bus_executables_refuse_before_any_daemon(self):
        bare = {k: v for k, v in os.environ.items()}
        bare['PATH'] = '/usr/bin:/bin'  # no Homebrew dbus-daemon/dbus-send
        record, outcome, report = self.custody('roundtrip', name='absent', env=bare)
        self.assertIn('command exited', str(outcome))
        self.assertEqual(record['status'], 'clean')  # custody clean; resource setup refused
        self.assertIsNone(report)
        self.assertIn(b'private service executable unavailable: dbus-daemon', (self.lane / 'absent.log').read_bytes())
        self.assertFalse((self.lane / 'services/services.json').exists())
        self.assertFalse(self.root.exists())

    def runner(self, mode, name):
        script = f'''
import signal, sys
sys.path.insert(0, {str(SCRIPTS)!r})
from pathlib import Path
from test_gate_darwin import darwin_command
def stop(signum, frame):
    raise KeyboardInterrupt()
signal.signal(signal.SIGTERM, stop)
records = []
try:
    darwin_command([sys.executable, '-c', {TARGET!r}, {str(self.root)!r}, {str(self.lane / 'services')!r},
                    {mode!r}, {str(self.work / (name + '-report.json'))!r}],
                   helper=Path({str(HELPER)!r}), records=records, errors=[], log=Path({str(self.lane / (name + '.log'))!r}))
except BaseException as exc:
    print(type(exc).__name__, exc, flush=True)
    sys.exit(3)
'''
        return subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def wait_report(self, name):
        path = self.work / (name + '-report.json')
        deadline = time.monotonic() + 60
        while not path.exists():
            self.assertLess(time.monotonic(), deadline, 'target never reported')
            time.sleep(0.05)
        time.sleep(0.2)
        return json.loads(path.read_text())

    def finish_runner(self, runner, name):
        output, _ = runner.communicate(timeout=240)
        self.assertEqual(runner.returncode, 3, output)
        record = json.loads((self.lane / (name + '.log.darwin-adapter.json')).read_text())
        if record['status'] in ('clean', 'recovered_lane_failed'):
            self.services.release_resource_directory(self.lane)
        return record, output

    def test_cancellation_tears_down_bus_root_and_escapee(self):
        runner = self.runner('hold', 'cancel')
        report = self.wait_report('cancel')
        runner.send_signal(signal.SIGTERM)
        record, output = self.finish_runner(runner, 'cancel')
        self.assertIn(b'KeyboardInterrupt', output)
        self.assertEqual(record['status'], 'clean')
        self.assertTrue(record['control_final']['cancelled'])
        for pid in report['escaped'] + [report['daemon_pid']]:
            self.assertFalse(alive(pid))
        self.assertEqual(self.journal()[-1], 'released')
        self.assertFalse(self.root.exists())

    def test_helper_death_recovery_cleans_bus_and_lane_stays_failed(self):
        runner = self.runner('hold', 'death')
        report = self.wait_report('death')
        os.kill(report['supervisor'], signal.SIGKILL)  # injected abnormal helper death (test only)
        record, output = self.finish_runner(runner, 'death')
        self.assertIn(b'recovered_lane_failed', output)
        self.assertEqual((record['status'], record['recovered']), ('recovered_lane_failed', True))
        self.assertEqual(record['recovery_final']['tag_authority'], 'parent-bound')
        for pid in report['escaped'] + [report['daemon_pid']]:
            self.assertFalse(alive(pid))
        self.assertEqual(self.journal()[-1], 'released')
        self.assertFalse(self.root.exists())

    def test_unvalidated_exit_retains_owned_directory(self):
        # A helper that never runs (adapter cannot validate) must not release files.
        from test_gate_darwin import darwin_command
        records = []
        with self.assertRaises(RuntimeError):
            darwin_command([sys.executable, '-c', 'pass'], helper=self.work / 'missing-helper.py',
                           records=records, errors=[], log=self.lane / 'missing.log')
        self.assertNotIn(records[0]['status'], ('clean', 'recovered_lane_failed'))
        self.services.retain_resource_directory(self.lane, 'test: helper missing')
        self.assertEqual(self.journal()[-1], 'retained')
        self.assertTrue(self.root.exists())
        with self.assertRaisesRegex(self.services.ServiceError, 'does not authorize recursive release'):
            self.services.release_resource_directory(self.lane)

    def test_strict_asan_ubsan_gio_consumer_on_private_bus(self):
        cflags = subprocess.run(['pkg-config', '--cflags', '--libs', 'gio-2.0'], capture_output=True, text=True)
        version = subprocess.run(['/usr/bin/clang', '--version'], capture_output=True, text=True).stdout
        if cflags.returncode or 'Apple clang' not in version:
            self.fail('strict ASan consumer toolchain (Apple clang + gio pkg-config) unavailable')
        source, binary = self.work / 'consumer.c', self.work / 'consumer'
        source.write_text(CONSUMER)
        subprocess.run(['/usr/bin/clang', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-fno-sanitize-recover=undefined', str(source), '-o', str(binary), *cflags.stdout.split()],
                       check=True, capture_output=True, timeout=120)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:strict_string_checks=1:halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        record, outcome, report = self.custody('asan', str(binary), name='asan', env=env)
        self.assertIsNone(outcome, record)
        code, stdout, stderr = report['asan']
        self.assertEqual(code, 0, stderr)
        self.assertIn('asan-consumer bus id ' + report['bus_id'], stdout)
        self.assertNotIn('AddressSanitizer', stderr)
        self.assertNotIn('runtime error', stderr)
        self.assertFalse(self.root.exists())


if __name__ == '__main__':
    unittest.main()
