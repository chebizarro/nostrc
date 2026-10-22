"""Fake Docker lifecycle proofs, NOT live MDK/peer acceptance."""
import json
import os
from pathlib import Path
import importlib.util
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'scripts/mdk-gate.py'
FAKE = r'''#!/usr/bin/env python3
import hashlib,json,os,pathlib,subprocess,sys,time
args=sys.argv[1:]
with open(os.environ['TRACE'],'a') as f: f.write(json.dumps(args)+'\n')
root=pathlib.Path(os.environ['TRACE']+'.daemon'); root.mkdir(exist_ok=True)
for d in ('containers','volumes','images'): (root/d).mkdir(exist_ok=True)
def save(d,k,v):
 p=root/d/k;tmp=p.with_suffix('.'+str(os.getpid()));tmp.write_text(json.dumps(v));tmp.replace(p)
def read(d,k):return json.loads((root/d/k).read_text())
def containers():return [json.loads(p.read_text()) for p in (root/'containers').iterdir() if p.is_file() and '.' not in p.name]
def find(k):return next(c for c in containers() if k in (c['Id'],c['Name'].lstrip('/')))
def cid(name):return hashlib.sha256(name.encode()).hexdigest()
def value(flag):return args[args.index(flag)+1]
def labels():return dict(args[i+1].split('=',1) for i,a in enumerate(args) if a=='--label')
buildkit='sha256:'+'b'*64
if args[0]=='uncertain-api':
 subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'])
 time.sleep(30);sys.exit(0)
if args[0]=='info':
 if args[-1]=='{{json .ClientInfo.Plugins}}':print(json.dumps([{'Name':'buildx','Path':sys.argv[0]}]))
 elif '--format' in args:print('fake-daemon')
 sys.exit(0)
if args[:2]==['context','show']:print('fake-context');sys.exit(0)
if args[:2]==['context','inspect']:print(json.dumps({'Host':'unix:///fake-docker.sock'}));sys.exit(0)
if args[:2]==['buildx','version']:
 if os.environ.get('NO_BUILDX'):sys.exit(42)
 print('fake-buildx');sys.exit(0)
if args[:2]==['buildx','create']:
 name=value('--name');node=value('--node');opts=dict(args[i+1].split('=',1) for i,a in enumerate(args) if a=='--driver-opt')
 p=pathlib.Path(os.environ['BUILDX_CONFIG'])/'instances';p.mkdir(parents=True)
 (p/name).write_text(json.dumps({'Name':name,'Driver':'docker-container','Nodes':[{'Name':node,'Endpoint':args[-1],'DriverOpts':opts}]}))
 print(name);sys.exit(0)
if args[:2]==['buildx','inspect']:
 name=args[2];data=json.loads((pathlib.Path(os.environ['BUILDX_CONFIG'])/'instances'/name).read_text());node=data['Nodes'][0];n='buildx_buildkit_'+node['Name'];volume=n+'_state'
 save('volumes',volume,{'Name':volume})
 save('containers',cid(n),{'Id':cid(n),'Name':'/'+n,'Image':buildkit,'Config':{'Env':['NOSTRC_MDK_RUN='+node['DriverOpts']['env.NOSTRC_MDK_RUN']],'Labels':{}},'Mounts':[{'Type':'volume','Name':volume,'Destination':'/var/lib/buildkit'}],'HostConfig':{'RestartPolicy':{'Name':'no'},'PidMode':'','Binds':None},'State':{'Running':True,'Pid':1}})
 print('running');sys.exit(0)
if args[:2]==['buildx','rm']:
 p=pathlib.Path(os.environ['BUILDX_CONFIG'])/'instances'/args[-1];data=json.loads(p.read_text());n='buildx_buildkit_'+data['Nodes'][0]['Name']
 (root/'containers'/cid(n)).unlink(missing_ok=True);(root/'volumes'/(n+'_state')).unlink(missing_ok=True);p.unlink();sys.exit(0)
if args[:2]==['image','inspect']:
 image=args[-1]
 if image=='moby/buildkit:buildx-stable-1':d={'Id':buildkit,'RepoDigests':['moby/buildkit@'+buildkit],'Config':{'Labels':{}}}
 else:
  d=read('images',image.replace(':','-'))
  if os.environ.get('MISMATCH_ID'):d['Id']='sha256:'+'0'*64
 print(json.dumps([d]));sys.exit(0)
if args[:2]==['buildx','build']:
 name=value('--builder');data=json.loads((pathlib.Path(os.environ['BUILDX_CONFIG'])/'instances'/name).read_text());node=data['Nodes'][0]['Name'];builder=cid('buildx_buildkit_'+node)
 if os.environ.get('BLOCK_BUILD'):
  pathlib.Path(os.environ['READY']).touch()
  while (root/'containers'/builder).exists() and read('containers',builder)['State']['Running']:time.sleep(.01)
  sys.exit(137)
 if os.environ.get('FAIL_BUILD'):sys.exit(23)
 if os.environ.get('NO_IID'):sys.exit(0)
 ls=labels();iid='sha256:'+hashlib.sha256(pathlib.Path(args[-1],'Dockerfile').read_bytes()+json.dumps(ls,sort_keys=True).encode()).hexdigest()
 pathlib.Path(value('--iidfile')).write_text(iid)
 pathlib.Path(value('--metadata-file')).write_text(json.dumps({'containerimage.digest':iid,'containerimage.descriptor':{'digest':iid},'buildx.build.ref':('sibling' if os.environ.get('BAD_METADATA') else name)+'/'+node+'/fake','image.name':value('-t')}))
 save('images',iid.replace(':','-'),{'Id':iid,'Config':{'Labels':ls}})
 if os.environ.get('PIPE_ONCE') and not pathlib.Path(os.environ['PIPE_READY']).exists():
  subprocess.Popen([sys.executable,'-c','import os,pathlib,sys,time;pathlib.Path(sys.argv[1]).write_text(str(os.getpid()));time.sleep(60)',os.environ['PIPE_READY']],start_new_session=True,stderr=subprocess.DEVNULL)
 sys.exit(0)
if args[:2]==['container','inspect']:
 try:print(json.dumps([find(args[-1])]))
 except StopIteration:sys.exit(1)
 sys.exit(0)
if args[0]=='ps':
 name=next(a[len('name=^/'): -1] for a in args if a.startswith('name=^/'))
 for d in containers():
  if d['Name']=='/'+name:print(d['Id'])
 sys.exit(0)
if args[:2]==['volume','ls']:
 name=next(a[len('name=^'): -1] for a in args if a.startswith('name=^'))
 if (root/'volumes'/name).exists():print(name)
 sys.exit(0)
if args[0]=='exec':print('ID PLATFORMS\nfake linux/arm64');sys.exit(0)
if args[0]=='stop':
 d=find(args[-1]);d['State']={'Running':False,'Pid':0};save('containers',d['Id'],d);sys.exit(0)
if args[0]=='logs':print('simulated BuildKit logs');sys.exit(0)
if args[0]=='create':
 name=value('--name');image=args[-1];ls=dict(read('images',image.replace(':','-'))['Config']['Labels'],**labels())
 d={'Id':cid(name),'Name':'/'+name,'Image':image,'Config':{'Labels':ls},'State':{'Running':False,'Pid':0}}
 if os.environ.get('MISMATCH_PEER'):d['Config']['Labels']['org.nostrc.mdk.run']='unrelated-owner'
 save('containers',cid(name),d);pathlib.Path(value('--cidfile')).write_text(cid(name))
 if '-v' in args:
  directory=pathlib.Path(value('-v').split(':')[0]);directory.mkdir(exist_ok=True);(directory/'vectors.jsonl').write_text('{"simulated":true}\n')
 if os.environ.get('SLOW_CREATE'):
  pathlib.Path(os.environ['READY']).touch()
  while not pathlib.Path(os.environ['RELEASE']).exists():time.sleep(.01)
 print(cid(name));sys.exit(0)
if args[0]=='start':
 d=find(args[-1]);d['State']={'Running':True,'Pid':1};save('containers',d['Id'],d)
 if os.environ.get('BLOCK_RUN'):
  pathlib.Path(os.environ['READY']).touch()
  while (root/'containers'/d['Id']).exists():time.sleep(.01)
  sys.exit(137)
 d['State']={'Running':False,'Pid':0};save('containers',d['Id'],d);sys.exit(0)
if args[:2]==['rm','-f']:
 try:(root/'containers'/find(args[-1])['Id']).unlink()
 except StopIteration:sys.exit(1)
 sys.exit(0)
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
        self.assertTrue(receipt['docker_lifecycle']['cleanup_complete'])
        self.assertEqual(receipt['docker_lifecycle']['scope'], 'original_docker_build_and_recorded_peers_only')

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
        runs = [x for x in self.traces() if x[0]=='create']
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
        self.assertEqual(len([x for x in self.traces() if x[:2]==['buildx','build']]),4)
        runs = [x for x in self.traces() if x[0]=='create']
        self.assertEqual(len({x[-1] for x in runs}),4)

    def test_partial_build_cannot_reuse_existing_tag(self):
        child = self.start(self.root('partial'),FAIL_BUILD='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(self.receipts()[0]['image_results'][0]['build_complete'])
        self.assertFalse(any(x[0]=='create' for x in self.traces()))

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
        self.assertFalse(any(x[0]=='create' for x in self.traces()))

    def test_wrong_image_inspection_refuses_consumer(self):
        child = self.start(self.root('mismatched-id'),MISMATCH_ID='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(self.receipts()[0]['image_results'][0]['build_complete'])
        self.assertFalse(any(x[0]=='create' for x in self.traces()))

    def test_context_mutation_refuses_consumer(self):
        child = self.start(self.root('mutated'),MUTATE_CONTEXT='1')
        self.assertNotEqual(child.wait(timeout=10),0)
        self.assertFalse(any(x[0]=='create' for x in self.traces()))

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

    def test_unsettled_admission_refuses_resource_removal(self):
        spec = importlib.util.spec_from_file_location('mdk_fence_under_test', HELPER)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        state = self.base / 'fence-state'
        state.mkdir()
        owner = helper.OwnedBuilder(dict(state=str(state), run_id='test-fence'))
        owner.started = True
        with mock.patch.object(helper, 'lock', side_effect=ValueError('admission still held')), mock.patch.object(helper, 'cli') as cli:
            result = owner.cleanup()
        cli.assert_not_called()
        self.assertFalse(result['cleanup_complete'])
        self.assertTrue((state / 'drain-requested.json').is_file())
        self.assertIn('resource removal refused', ' '.join(result['errors']))

    def test_timed_out_api_with_child_writer_cannot_claim_cleanup(self):
        spec = importlib.util.spec_from_file_location('mdk_timeout_under_test', HELPER)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        state = self.base / 'api-state'
        state.mkdir()
        (state / 'containers').mkdir()
        (state / 'clients').mkdir()
        config = dict(state=str(state), run_id='test-timeout', docker=str(self.bin / 'docker'),
                      docker_endpoint='unix:///fake-docker.sock', daemon_id='fake-daemon', images={})
        start = time.monotonic()
        with mock.patch.dict(os.environ, self.env):
            with self.assertRaises(subprocess.TimeoutExpired):
                helper.cli(config, ['uncertain-api'], timeout=0.2)
            result = helper.OwnedBuilder(config).cleanup()
        self.assertLess(time.monotonic() - start, 5, 'CLI descendant pipe kept timeout unbounded')
        self.assertFalse(result['cleanup_complete'])
        self.assertIn('unresolved Docker API request', ' '.join(result['errors']))

    def test_slow_create_settles_before_cleanup_inventory(self):
        ready, release = self.base / 'creating', self.base / 'release-create'
        child = self.start(self.root('slow-create'), SLOW_CREATE='1', READY=str(ready), RELEASE=str(release))
        self.await_path(ready)
        child.terminate()
        state, = self.evidence.iterdir()
        self.await_path(state / 'drain-requested.json')
        self.assertIsNone(child.poll())
        release.touch()
        self.assertEqual(child.wait(timeout=15), 130)
        receipt, = self.receipts()
        self.assertTrue(receipt['docker_lifecycle']['cleanup_complete'], receipt)
        self.assertTrue(receipt['containers'][0]['removed'])
        self.assertEqual(receipt['containers'][0]['status'], 'starting')
        # The already-admitted create completed; start was fenced, not launched.
        self.assertFalse(any(x[0] == 'start' for x in self.traces()))

    def test_missing_buildx_refuses_before_allocation_and_command(self):
        child = self.start(self.root('no-buildx'), NO_BUILDX='1')
        self.assertEqual(child.wait(timeout=10), 1)
        receipt, = self.receipts()
        self.assertEqual(receipt['command_status'], 'not_started')
        self.assertFalse(receipt['cleanup_complete'])
        self.assertFalse(any(x[:2] in (['buildx', 'create'], ['buildx', 'build']) for x in self.traces()))
        self.assertIn('42', receipt['error'])

    def test_exact_builder_metadata_required_before_consumer(self):
        child = self.start(self.root('bad-metadata'), BAD_METADATA='1')
        self.assertEqual(child.wait(timeout=10), 1)
        receipt, = self.receipts()
        self.assertFalse(receipt['image_results'][0]['build_complete'])
        self.assertTrue(receipt['docker_lifecycle']['cleanup_complete'])
        self.assertFalse(any(x[0] == 'create' for x in self.traces()))

    def test_peer_identity_mismatch_retains_cid_and_refuses_removal(self):
        child = self.start(self.root('bad-peer'), MISMATCH_PEER='1')
        self.assertEqual(child.wait(timeout=10), 1)
        receipt, = self.receipts()
        self.assertFalse(receipt['docker_lifecycle']['cleanup_complete'])
        peer, = receipt['containers']
        self.assertRegex(peer['container_id'], '^[0-9a-f]{64}$')
        self.assertIn('peer CID/image/labels mismatch', ' '.join(receipt['cleanup_errors']))
        self.assertFalse(any(x[:2] == ['rm', '-f'] for x in self.traces()))

    def test_fixed_coordinator_all_versions_has_only_scoped_success(self):
        root = self.root('images')
        result = subprocess.run([sys.executable, str(root / 'scripts/mdk-gate.py'), 'images',
            '--source-root', str(root), '--evidence-dir', str(self.evidence),
            '--version', '08', '--version', '09', '--version', '011', '--probe'],
            env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt, = self.receipts()
        self.assertEqual(receipt['status'], 'docker_lifecycle_succeeded')
        self.assertTrue(receipt['cleanup_complete'])
        self.assertFalse(receipt['complete'])
        self.assertEqual(len(receipt['image_results']), 3)
        self.assertEqual(len(receipt['containers']), 3)
        for args in self.traces():
            self.assertNotIn('--use', args)
            self.assertNotIn('prune', args)
            if args[:2] == ['buildx', 'build']:
                self.assertEqual(args[args.index('--builder') + 1], receipt['builder']['name'])

    def test_cancel_removes_only_run_owned_container(self):
        ready = self.base/'run-ready'
        first = self.start(self.root('cancel-run'),BLOCK_RUN='1',READY=str(ready))
        self.await_path(ready)
        first.send_signal(signal.SIGTERM)
        self.assertEqual(first.wait(timeout=10),130)
        receipt = self.receipts()[0]
        removed = [x[-1] for x in self.traces() if x[:2]==['rm','-f']]
        self.assertEqual(removed,[x['container_id'] for x in receipt['containers']])
        self.assertEqual(receipt['status'],'cancelled')
        self.assertNotIn('prune',str(self.traces()))


if __name__=='__main__': unittest.main()
