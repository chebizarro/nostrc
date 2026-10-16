"""Fake Docker lifecycle proofs, NOT live MDK/peer acceptance."""
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'scripts/mdk-gate.py'
FAKE = r'''#!/usr/bin/env python3
import hashlib,json,os,pathlib,sys,time
args=sys.argv[1:]
with open(os.environ['TRACE'],'a') as f: f.write(json.dumps(args)+'\n')
if args[0]=='info': sys.exit(0)
if args[:2]==['image','inspect']: print('sha256:'+'0'*64 if os.environ.get('MISMATCH_ID') else args[-1]); sys.exit(0)
if args[0]=='build':
    if os.environ.get('BLOCK_BUILD'):
        pathlib.Path(os.environ['READY']).touch()
        while True: time.sleep(1)
    if os.environ.get('FAIL_BUILD'): sys.exit(23)
    if os.environ.get('NO_IID'): sys.exit(0)
    iid='sha256:'+hashlib.sha256(pathlib.Path(args[-1],'Dockerfile').read_bytes()).hexdigest()
    pathlib.Path(args[args.index('--iidfile')+1]).write_text(iid)
    sys.exit(0)
if args[0]=='run':
    if '-v' in args:
        directory=pathlib.Path(args[args.index('-v')+1].split(':')[0]); directory.mkdir(exist_ok=True)
        (directory/'vectors.jsonl').write_text('{"simulated":true}\n')
    if os.environ.get('BLOCK_RUN'):
        pathlib.Path(os.environ['READY']).touch()
        while True: time.sleep(1)
    sys.exit(0)
if args[:2]==['rm','-f']: sys.exit(0)
sys.exit(99)
'''
DRIVER = r'''
import json,os,pathlib,subprocess,time
state=pathlib.Path(os.environ['NOSTRC_MDK_STATE'])
cfg=json.loads((state/'config.json').read_text())
for v,d in cfg['images'].items():
    subprocess.run([os.environ['NOSTRC_MDK_DOCKER'],'build','-t',d['tag'],d['context']],check=True)
    if os.environ.get('MUTATE_CONTEXT'): pathlib.Path(d['context'],'Dockerfile').write_text('changed')
    mount=['-v',os.environ['ARTIFACT_DIR']+':/artifacts'] if os.environ.get('ARTIFACT_DIR') else []
    subprocess.run([os.environ['NOSTRC_MDK_DOCKER'],'run','--rm','-i','--network','host',*mount,d['tag']],check=True)
    if os.environ.get('ARTIFACT_DIR'): pathlib.Path(os.environ['ARTIFACT_DIR'],'vectors.jsonl').write_text('reset')
if os.environ.get('HOLD'):
    pathlib.Path(os.environ['READY']).touch()
    while not pathlib.Path(os.environ['RELEASE']).exists(): time.sleep(.02)
'''


class MDKGateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.bin = self.base/'bin'
        self.bin.mkdir()
        (self.bin/'docker').write_text(FAKE)
        (self.bin/'docker').chmod(0o700)
        self.trace = self.base/'trace'
        self.env = dict(os.environ, PATH=str(self.bin)+os.pathsep+os.environ['PATH'], TRACE=str(self.trace))
        self.evidence = self.base/'evidence'

    def root(self, name):
        root = self.base/name
        (root/'scripts').mkdir(parents=True)
        shutil.copyfile(HELPER,root/'scripts/mdk-gate.py')
        for d in ('driver','driver-0.9-probe','driver-0.11'):
            path = root/'tests/interop/mdk'/d
            path.mkdir(parents=True)
            (path/'Dockerfile').write_text(name+d)
        return root

    def start(self, root, versions=('011',), **env):
        command = [sys.executable,str(root/'scripts/mdk-gate.py'),'run','--source-root',str(root),'--evidence-dir',str(self.evidence)]
        for version in versions: command += ['--version',version]
        command += ['--',sys.executable,'-c',DRIVER]
        child = subprocess.Popen(command,env=dict(self.env,**env),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: self.stop(child))
        return child

    def stop(self, child):
        if child.poll() is None:
            child.terminate()
            child.wait(timeout=10)

    def await_path(self, path):
        deadline = time.monotonic()+10
        while not path.exists():
            if time.monotonic()>deadline: self.fail('readiness event missing: '+str(path))
            time.sleep(.01)

    def traces(self):
        return [json.loads(x) for x in self.trace.read_text().splitlines()]

    def receipts(self):
        return [json.loads(x.read_text()) for x in self.evidence.glob('*/receipt.json')]

    def assert_command_completed_without_tree_proof(self, receipt):
        self.assertEqual(receipt['status'], 'cleanup_unverified')
        self.assertEqual(receipt['exit_code'], 1)
        self.assertEqual(receipt['command_status'], 'succeeded')
        self.assertEqual(receipt['command_exit_code'], 0)
        self.assertFalse(receipt['cleanup_complete'])
        self.assertEqual(receipt['cleanup_results'], dict(original_session='quiescent',
            recorded_containers='removed', arbitrary_process_tree='unverified'))
        self.assertFalse(receipt['complete'])

    def test_two_roots_different_inputs_bind_exact_ids(self):
        a,b = self.root('a'),self.root('b')
        children = [self.start(a),self.start(b)]
        self.assertEqual([p.wait(timeout=15) for p in children],[1,1])
        receipts = self.receipts()
        self.assertEqual(len(receipts),2)
        for receipt in receipts:
            self.assert_command_completed_without_tree_proof(receipt)
        ids = {r['image_results'][0]['image_id'] for r in receipts}
        tags = {r['images']['011']['tag'] for r in receipts}
        self.assertEqual(len(ids),2)
        self.assertEqual(len(tags),2)
        runs = [x for x in self.traces() if x[0]=='run']
        self.assertEqual({x[-1] for x in runs},ids)
        self.assertTrue(all('--name' in x and '--label' in x for x in runs))
        self.assertTrue(all(not r['complete'] for r in receipts))

    def test_fixed_tags_lock_through_consumers_and_cancel_waiter(self):
        for version in ('08','09'):
            ready,release = self.base/('ready'+version),self.base/('release'+version)
            first = self.start(self.root('first'+version),(version,),HOLD='1',READY=str(ready),RELEASE=str(release))
            self.await_path(ready)
            other = self.root('second'+version)
            second = self.start(other,(version,))
            # Require evidence of real LOCK_NB contention, not merely a started
            # process that could be cancelled before ever attempting the lock.
            deadline = time.monotonic()+10
            while not list(self.evidence.glob('*/'+version+'-waiting.json')):
                if time.monotonic()>deadline: self.fail('second supervisor never contended for fixed-tag lock')
                time.sleep(.01)
            second.terminate()
            self.assertEqual(second.wait(timeout=10),130)
            self.assertIsNone(first.poll())
            release.touch()
            self.assertEqual(first.wait(timeout=10),1)
            self.assertEqual(self.start(other,(version,)).wait(timeout=10),1)
            for receipt in self.receipts():
                if receipt['command_exit_code'] == 0:
                    self.assert_command_completed_without_tree_proof(receipt)
        self.assertEqual(len([x for x in self.traces() if x[0]=='build']),4)
        runs = [x for x in self.traces() if x[0]=='run']
        self.assertEqual(len({x[-1] for x in runs}),4)

    def test_partial_build_cannot_reuse_existing_tag(self):
        child = self.start(self.root('partial'),FAIL_BUILD='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(self.receipts()[0]['image_results'][0]['build_complete'])
        self.assertFalse(any(x[0]=='run' for x in self.traces()))

    def test_vectors_preserved_before_later_fixture_reset(self):
        child = self.start(self.root('vectors'),ARTIFACT_DIR=str(self.base/'artifacts'))
        self.assertEqual(child.wait(timeout=10),1)
        receipt = self.receipts()[0]
        self.assert_command_completed_without_tree_proof(receipt)
        artifact = Path(receipt['containers'][0]['artifact'])
        self.assertEqual(artifact.read_text(),'{"simulated":true}\n')
        self.assertEqual((self.base/'artifacts/vectors.jsonl').read_text(),'reset')

    def test_missing_iid_is_not_a_completed_build(self):
        child = self.start(self.root('no-iid'),NO_IID='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(self.receipts()[0]['image_results'][0]['build_complete'])
        self.assertFalse(any(x[0]=='run' for x in self.traces()))

    def test_wrong_image_inspection_refuses_consumer(self):
        child = self.start(self.root('mismatched-id'),MISMATCH_ID='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(self.receipts()[0]['image_results'][0]['build_complete'])
        self.assertFalse(any(x[0]=='run' for x in self.traces()))

    def test_context_mutation_refuses_consumer(self):
        child = self.start(self.root('mutated'),MUTATE_CONTEXT='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(any(x[0]=='run' for x in self.traces()))

    def test_cancelled_build_releases_fixed_lock(self):
        root = self.root('cancel-build')
        ready = self.base/'build-ready'
        first = self.start(root,('08',),BLOCK_BUILD='1',READY=str(ready))
        self.await_path(ready)
        first.terminate()
        self.assertEqual(first.wait(timeout=10),130)
        self.assertFalse(self.receipts()[0]['image_results'][0]['build_complete'])
        self.assertEqual(self.start(root,('08',)).wait(timeout=10),1)
        for receipt in self.receipts():
            if receipt['command_exit_code'] == 0:
                self.assert_command_completed_without_tree_proof(receipt)

    def test_cancel_removes_only_run_owned_container(self):
        ready = self.base/'run-ready'
        first = self.start(self.root('cancel-run'),BLOCK_RUN='1',READY=str(ready))
        self.await_path(ready)
        first.send_signal(signal.SIGTERM)
        self.assertEqual(first.wait(timeout=10),130)
        receipt = self.receipts()[0]
        removed = [x[-1] for x in self.traces() if x[:2]==['rm','-f']]
        self.assertEqual(removed,[x['name'] for x in receipt['containers']])
        self.assertEqual(receipt['status'],'cancelled')
        self.assertNotIn('prune',str(self.traces()))


if __name__=='__main__': unittest.main()
