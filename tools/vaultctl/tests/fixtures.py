#!/usr/bin/env python3
"""Real CLI + real pool/libblossom against bounded loopback HTTP/WS fixtures.
No lab/production acceptance is inferred. All services and private state are temporary.
"""
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)
def add(a, b):
    if a is None: return b
    if b is None: return a
    x, y = a; u, v = b
    if x == u and (y != v or not y): return None
    m = ((3*x*x if a == b else v-y) * pow(2*y if a == b else u-x, -1, P)) % P
    z = (m*m-x-u) % P
    return z, (m*(x-z)-y) % P

def mul(n, a):
    r = None
    while n:
        if n & 1: r = add(r, a)
        a = add(a, a); n >>= 1
    return r

def pub(sk): return f"{mul(sk, G)[0]:064x}"
def verify(e):
    encoded = json.dumps([0,e['pubkey'],e['created_at'],e['kind'],e['tags'],e['content']], separators=(',', ':'), ensure_ascii=False).encode()
    digest = hashlib.sha256(encoded).digest()
    assert digest.hex() == e['id']
    x = int(e['pubkey'],16); assert x < P
    y = pow((x*x*x+7)%P, (P+1)//4,P); assert y*y%P == (x*x*x+7)%P
    if y&1: y=P-y
    sig = bytes.fromhex(e['sig']); assert len(sig)==64
    r,s = int.from_bytes(sig[:32],'big'),int.from_bytes(sig[32:],'big'); assert r<P and s<N
    tag=hashlib.sha256(b'BIP0340/challenge').digest()
    challenge=int.from_bytes(hashlib.sha256(tag+tag+sig[:32]+bytes.fromhex(e['pubkey'])+digest).digest(),'big')%N
    check=add(mul(s,G),mul(N-challenge,(x,y)))
    assert check is not None and not(check[1]&1) and check[0]==r

class Lab(http.server.ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self):
        super().__init__(('127.0.0.1',0),Handler)
        self.url = f'http://127.0.0.1:{self.server_port}'
        self.ws = f'ws://127.0.0.1:{self.server_port}/relay'
        self.blobs={}; self.events={}; self.failures=[]; self.auths=0; self.reqs=0; self.attempts=[]; self.puts=[]
        self.reject=False; self.closed=False; self.omit_eose=False; self.uncertain=False; self.bad_blob=False; self.hidden_auth=False; self.fail_manifest=False; self.saw_large=False
        self.lock=threading.Lock();self.thread=threading.Thread(target=self.serve_forever,daemon=True);self.thread.start()
    def finish(self): self.shutdown();self.server_close();self.thread.join()

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args): pass
    def response(self,status,body,ct='application/json'):
        if isinstance(body,dict): body=json.dumps(body).encode()
        self.send_response(status);self.send_header('Content-Type',ct);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    def do_GET(self):
        try:
            if self.headers.get('Upgrade','').lower()=='websocket': return self.websocket()
            if self.path=='/relay': return self.response(200,{'limitation':{'auth_required':not self.server.hidden_auth},'vault':True,'supported_nips':[11,42]})
            if self.path=='/': return self.response(200,{'private':True,'supported_buds':[1,2]})
            h=self.path[1:]
            with self.server.lock: b=self.server.blobs.get(h)
            if b is None:return self.response(404,b'')
            if self.server.bad_blob:b=b'corrupt'
            self.response(200,b,'application/octet-stream')
        except (BrokenPipeError,ConnectionResetError): pass
        except Exception as e: self.server.failures.append(repr(e));raise
    def do_HEAD(self):
        with self.server.lock: b=self.server.blobs.get(self.path[1:])
        self.send_response(200 if b is not None else 404);self.send_header('Content-Length',str(len(b) if b else 0));self.end_headers()
    def do_PUT(self):
        try:
            assert self.path=='/upload'
            b=self.rfile.read(int(self.headers['Content-Length']));h=hashlib.sha256(b).hexdigest()
            auth=self.headers['Authorization'];assert auth.startswith('Nostr ')
            e=json.loads(base64.b64decode(auth[6:]));verify(e);assert e['kind']==24242
            assert ['t','upload'] in e['tags'] and ['x',h] in e['tags']
            with self.server.lock:self.server.blobs[h]=b;self.server.puts.append(h)
            # This fixed input's CDC chunks after the root's tiny file are
            # all >10 KiB. Fail its following small directory-manifest PUT;
            # zero-budget resume below also proves all chunk jobs completed.
            if self.server.fail_manifest:
                if len(b)>10000:self.server.saw_large=True
                elif self.server.saw_large:
                    self.server.fail_manifest=False
                    return self.response(500,b'accepted bytes but manifest reply failed')
            self.response(200,{'sha256':h,'size':len(b),'url':self.server.url+'/'+h,'type':'application/octet-stream','uploaded':e['created_at']})
        except Exception as e:self.server.failures.append(repr(e));raise
    def exact(self,n):
        b=self.rfile.read(n)
        if len(b)!=n: raise EOFError()
        return b
    def recv(self):
        h=self.exact(2);op=h[0]&15;n=h[1]&127;mask=h[1]&128
        if n==126:n=struct.unpack('!H',self.exact(2))[0]
        elif n==127:n=struct.unpack('!Q',self.exact(8))[0]
        assert n<=1024*1024 and mask
        key=self.exact(4);b=self.exact(n);b=bytes(v^key[i%4] for i,v in enumerate(b))
        if op==8:raise EOFError()
        if op==9:self.frame(b,10);return None
        assert op==1
        return json.loads(b)
    def frame(self,b,op=1):
        if not isinstance(b,bytes):b=json.dumps(b,separators=(',',':')).encode()
        n=len(b);prefix=bytes([128|op,n]) if n<126 else bytes([128|op,126])+struct.pack('!H',n) if n<65536 else bytes([128|op,127])+struct.pack('!Q',n)
        self.wfile.write(prefix+b);self.wfile.flush()
    def websocket(self):
        key=self.headers['Sec-WebSocket-Key'];accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade');self.send_header('Sec-WebSocket-Accept',accept)
        if self.headers.get('Sec-WebSocket-Protocol'):self.send_header('Sec-WebSocket-Protocol',self.headers['Sec-WebSocket-Protocol'].split(',')[0])
        self.end_headers();self.wfile.flush();self.connection.settimeout(10);challenge='fixture-challenge';authed=False
        if not self.server.hidden_auth:self.frame(['AUTH',challenge])
        try:
            while True:
                msg=self.recv()
                if msg is None:continue
                if msg[0]=='AUTH':
                    e=msg[1];verify(e);assert e['kind']==22242;assert ['challenge',challenge] in e['tags'];assert ['relay',self.server.ws] in e['tags'];authed=True;self.server.auths+=1;self.frame(['OK',e['id'],True,''])
                elif msg[0]=='EVENT':
                    assert authed;e=msg[1];verify(e);assert e['kind'] in (30078,1059)
                    self.server.attempts.append(e['id'])
                    if self.server.reject:self.frame(['OK',e['id'],False,'blocked: fixture rejection']);continue
                    with self.server.lock:self.server.events[e['id']]=e
                    if self.server.uncertain:return
                    self.frame(['OK',e['id'],True,''])
                elif msg[0]=='REQ':
                    self.server.reqs+=1;sub=msg[1]
                    if not authed:
                        self.frame(['AUTH',challenge]);self.frame(['CLOSED',sub,'auth-required: authenticate first']);continue
                    if self.server.closed:self.frame(['CLOSED',sub,'blocked: fixture refusal']);continue
                    f=msg[2]
                    assert 'ids' in f or (f['kinds']==[30078] and f['authors'] and len(f['#d'])==1)
                    with self.server.lock:events=list(self.server.events.values())
                    if 'ids' not in f:
                        latest={}
                        for item in events:
                            if item['kind']!=30078:continue
                            d=next(t[1] for t in item['tags'] if t[0]=='d')
                            key=(item['pubkey'],d);prior=latest.get(key)
                            if prior is None or item['created_at']>prior['created_at'] or (item['created_at']==prior['created_at'] and item['id']<prior['id']):latest[key]=item
                        events=list(latest.values())
                    for e in events:
                        if 'ids' in f and e['id'] not in f['ids']:continue
                        if 'authors' in f and e['pubkey'] not in f['authors']:continue
                        if 'kinds' in f and e['kind'] not in f['kinds']:continue
                        if '#d' in f and not any(t[0]=='d' and t[1] in f['#d'] for t in e['tags']):continue
                        self.frame(['EVENT',sub,e])
                    if not self.server.omit_eose:self.frame(['EOSE',sub])
                elif msg[0]=='CLOSE': pass
                else:raise AssertionError(msg[0])
        except (EOFError,ConnectionResetError,BrokenPipeError,socket.timeout):pass
        except Exception as e:self.server.failures.append(repr(e));raise
        finally:self.close_connection=True

def main():
    binary,helper=sys.argv[1:];lab=Lab()
    try:
        with tempfile.TemporaryDirectory(prefix='vaultctl-fixture-') as directory:
            base=Path(directory);os.chmod(base,0o700);source=base/'source';source.mkdir();(source/'nested').mkdir();(source/'empty').write_bytes(b'');(source/'trailing-zero').write_bytes(b'secret\x00\x00');(source/'nested'/'large').write_bytes(hashlib.shake_256(b'CDC fixture').digest(350000));os.symlink('trailing-zero',source/'link')
            key=base/'identity';key.write_text(f'{1:064x}');key.chmod(0o600);state=base/'state'
            common=['--state',str(state),'--key-file',str(key),'--allow-local','--deadline-ms','2500']
            def run(command,*args,expect=0):
                result=subprocess.run([binary,command,*common,*map(str,args)],capture_output=True,text=True,timeout=30)
                if result.returncode!=expect:raise AssertionError(f'{command} exit {result.returncode}, expected {expect}\n{result.stdout}\n{result.stderr}')
                assert not lab.failures,lab.failures
                if expect==0:return json.loads(result.stdout.splitlines()[-1])
                assert '"healthy":true' not in result.stdout
                return result
            shared=run('share','--relay',lab.ws,'--blossom',lab.url,source);assert shared['head'] in lab.events
            checked=run('fsck');assert checked['healthy'] and checked['nodes']==2 and checked['chunks']>=3
            folded=run('fold');assert folded['owner_view']==shared['head'] and folded['conflicts']==[]
            assert run('fold')==folded  # Replayed known heads preserve canonical IDs/output.
            recipient=pub(2);run('grant','--recipient',recipient,'--write')
            wrap=[e for e in lab.events.values() if e['kind']==1059][-1];event=base/'wrap.json';event.write_text(json.dumps(wrap));event.chmod(0o600)
            got=subprocess.run([helper,'grant',str(state),f'{2:064x}',str(event)],capture_output=True,text=True,timeout=10);assert got.returncode==0,(got.stdout,got.stderr)
            grant=json.loads(got.stdout);assert grant['cap']=='write'
            run('grant','--recipient',pub(3))
            # Two genuinely signed concurrent paths, not a mocked fold result.
            for sk in (1,2):
                fork=base/f'fork-{sk}.json'
                generated=subprocess.run([helper,'fork',str(state),f'{sk:064x}',str(fork)],capture_output=True,text=True,timeout=10)
                assert generated.returncode==0,(generated.stdout,generated.stderr)
                e=json.loads(fork.read_text());verify(e);lab.events[e['id']]=e
                for obj in (state/'objects').iterdir():lab.blobs[obj.name]=obj.read_bytes()
            conflict=run('fold');assert len(conflict['frontier'])==2 and conflict['conflicts']
            assert conflict['owner_view']==json.loads((base/'fork-1.json').read_text())['id']
            assert run('fold')==conflict
            # Lazy publishes the fresh epoch before any old chunks are migrated.
            run('revoke','--member',recipient,'--budget','0',expect=4)
            continuation=[e for e in lab.events.values() if e['kind']==1059][-1]
            event.write_text(json.dumps(continuation))
            received=subprocess.run([helper,'grant',str(state),f'{3:064x}',str(event)],capture_output=True,text=True,timeout=10)
            assert received.returncode==0,(received.stdout,received.stderr)
            assert json.loads(received.stdout)['ring'][0]['epoch']==2
            run('fsck');run('fold')
            before_migration=run('fold')['owner_view']
            lab.fail_manifest=True;lab.saw_large=False
            run('revoke','--resume','--budget','2000000',expect=5)
            # All chunks are current, but saved descriptor changes still need
            # publication even when a resumed task completes zero chunks.
            run('revoke','--resume','--reconcile','--budget','0')
            assert run('fold')['owner_view']!=before_migration
            run('fsck')
            # A current signed owner checkpoint remains authoritative when
            # replaceable relays no longer have its signed parent closure.
            def inject(mode):
                path=base/f'{mode}.json'
                generated=subprocess.run([helper,mode,str(state),f'{1:064x}',str(path)],capture_output=True,text=True,timeout=10)
                assert generated.returncode==0,(generated.stdout,generated.stderr)
                e=json.loads(path.read_text());verify(e);lab.events[e['id']]=e
                for obj in (state/'objects').iterdir():lab.blobs[obj.name]=obj.read_bytes()
                return e
            broken=inject('missing');attempts=len(lab.attempts)
            run('grant','--recipient',pub(8));assert len(lab.attempts)>=attempts+2
            del lab.events[broken['id']]
            # Removing a directory from the latest snapshot must not discard its
            # epoch history needed by retained snapshots. Metadata-only grant
            # also must not require/re-upload any uncached referenced chunks.
            inject('prune')
            for obj in (state/'objects').iterdir():obj.unlink()
            uploads=len(lab.puts);run('grant','--recipient',pub(6))
            assert len(lab.puts)==uploads+1
            run('fsck')
            # Strict remote corruption: local ciphertext cache must not hide it.
            lab.bad_blob=True;run('fsck',expect=1);lab.bad_blob=False
            # Names and true lengths come exclusively from authenticated
            # manifest bodies, not private out-of-band sidecar directories.
            assert not (state/'lengths').exists() and not (state/'names').exists()
            run('fsck')
            # CLOSED and absent EOSE must be visible failures; no timed success.
            lab.closed=True;run('fold',expect=1);lab.closed=False
            lab.omit_eose=True;run('fold',expect=1);lab.omit_eose=False
            # An AUTH challenge/CLOSED can arrive only after REQ, despite a
            # non-auth NIP-11 advertisement: reissue after its matching AUTH OK.
            lab.hidden_auth=True;run('fold');lab.hidden_auth=False
            # ACK false is not success; resume uses the exact saved signed head.
            lab.reject=True;run('grant','--recipient',pub(4),expect=1);before=set(lab.events);rejected=lab.attempts[-1];lab.reject=False;run('grant','--resume');assert set(lab.events)>before
            assert lab.attempts.count(rejected)==2
            # Accepted-but-unacknowledged head: no implicit retries; reconcile by
            # exact-id EVENT plus EOSE before finishing the original operation.
            lab.uncertain=True;run('grant','--recipient',pub(5),expect=5);uncertain=lab.attempts[-1];lab.uncertain=False
            run('grant','--resume',expect=5);assert lab.attempts.count(uncertain)==1
            run('grant','--resume','--reconcile');assert lab.attempts.count(uncertain)==1
            run('fsck')
            # Eager budget refusal precedes any chunk upload or new head.
            attempts=len(lab.attempts);blobs=set(lab.blobs)
            run('revoke','--member',pub(4),'--eager','--budget','0',expect=4)
            assert len(lab.attempts)==attempts and set(lab.blobs)==blobs
            run('revoke','--resume','--budget','2000000');run('fsck')
            assert lab.auths>0 and lab.reqs>0 and not lab.failures
            print('OK share/grant/revoke/fold/fsck via actual pool, AUTH/OK/EOSE and libblossom; refusal checks')
    finally:lab.finish()
if __name__=='__main__':main()
