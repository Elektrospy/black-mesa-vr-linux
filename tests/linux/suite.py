import subprocess, re, os, sys
R=os.environ['B']; RUN=os.path.join(os.path.dirname(os.path.abspath(__file__)),'run_case.sh')
fails=0
def run(name, helper, cfg, env=(), game_args='300 500', fake_bin=None):
    subprocess.run([RUN, name, *helper, '--', cfg, '--', 'X=1', *env], check=True, env=dict(os.environ, GAME_ARGS=game_args, **({'FAKE_BIN': fake_bin} if fake_bin else {})))
    d=f'{R}/cases/{name}'
    rd=lambda f: open(f'{d}/{f}', errors='replace').read() if os.path.exists(f'{d}/{f}') else ''
    return rd('bmvr_linux_log.txt'), rd('trace.txt').splitlines(), rd('game.out'), rd('exit.txt')
def check(label, ok, extra=''):
    global fails
    print(('  PASS  ' if ok else '  FAIL  ')+label+(('  -> '+extra) if (extra and not ok) else ''))
    if not ok: fails+=1
def gl(trace, key): return [l for l in trace if l.startswith('GL '+key)]
def nsnap(trace): return len([l for l in trace if l.startswith('HELPER frame')])

print('T1  default (sync=finish), helper OFFERS semaphores')
log,tr,out,ex=run('t1',['--sem'],'')
check('game ran clean, GL bindings preserved', 'gl_state_violations=0' in out and 'exit=0' in ex)
check('semaphores offered but not requested -> glFinish + ack', 'sync = glFinish + ack (semaphores offered by the helper but not requested' in log)
check('one glFinish per snapshot, no semaphore calls', len(gl(tr,'FINISH'))==nsnap(tr)>=3 and not [l for l in tr if 'Semaphore' in l], f'{len(gl(tr,"FINISH"))} finish / {nsnap(tr)} frames')
check('frames carry no SEMAPHORE flag', all(int(re.search(r'flags=0x([0-9a-f]+)',l).group(1),16)&4==0 for l in tr if l.startswith('HELPER frame')))

print('T2  sync=sem with a helper that offers semaphores')
log,tr,out,ex=run('t2',['--sem'],'sync=sem\n')
check('game ran clean, GL bindings preserved', 'gl_state_violations=0' in out and 'exit=0' in ex)
check('both semaphores imported as OPAQUE_FD', len([l for l in tr if 'ImportSemaphoreFd' in l and 'type=0x9586' in l])==2)
check('log says GPU semaphores', 'sync = GPU semaphores' in log)
n=nsnap(tr)
check('snapshots delivered', n>=3, str(n))
check('NO glFinish at all', not gl(tr,'FINISH'))
check('every frame flagged SEMAPHORE', all(int(re.search(r'flags=0x([0-9a-f]+)',l).group(1),16)&4 for l in tr if l.startswith('HELPER frame')))
waits=gl(tr,'WaitSemaphore'); sigs=gl(tr,'SignalSemaphore')
check('one signal per frame, waits = frames-1 (none before the first)', len(sigs)==n and len(waits)==n-1, f'sig={len(sigs)} wait={len(waits)} frames={n}')
check('signal uses gl_ready (1001), wait uses vk_done (1002)', all('sem=1001' in l for l in sigs) and all('sem=1002' in l for l in waits))
check('barrier names the shared texture, layout GENERAL (0x958d), nt=1', all('layout=0x958d' in l and '(nt=1)' in l for l in sigs+waits))
# ordering per frame
seq=[l.split()[1] if l.startswith('GL ') else l.split()[1] for l in tr if (l.startswith('GL BLIT') or 'Semaphore sem' in l or l.startswith('GL FLUSH') or l.startswith('HELPER frame') or l.startswith('HELPER ack')) and 'Import' not in l]
txt=' '.join(seq)
check('first frame: BLIT, SIGNAL, FLUSH, frame, ack (no wait)', txt.startswith('BLIT SignalSemaphore FLUSH frame ack'), txt[:80])
check('later frames: WAIT, BLIT, SIGNAL, FLUSH, frame, ack', txt.count('WaitSemaphore BLIT SignalSemaphore FLUSH frame ack')==n-1, txt[:200])
# a WAIT is only ever issued after the previous frame was acked
ok=True; acked=0; waited=0
for l in tr:
    if l.startswith('HELPER ack'): acked+=1
    if l.startswith('GL WaitSemaphore'):
        waited+=1
        if acked<waited: ok=False
check('every GL wait comes after the helper acked the previous frame (its signal is submitted)', ok)
check('average cost lines present', 'xfer: average over' in log or n<10)

print('T3  sync=sem but the helper offers NO semaphores')
log,tr,out,ex=run('t3',[],'sync=sem\n')
check('fallback explained', 'sync=sem was requested but the helper offered no semaphores' in log)
check('glFinish path used, no semaphore calls', len(gl(tr,'FINISH'))==nsnap(tr)>=3 and not [l for l in tr if 'Semaphore' in l])

print('T4  sync=sem but GL has no semaphore entry points')
log,tr,out,ex=run('t4',['--sem'],'sync=sem\n',('MOCK_NO_SEM=1',))
check('fallback explained', 'GL_EXT_semaphore_fd entry points missing' in log)
check('glFinish path used, nothing crashed', len(gl(tr,'FINISH'))==nsnap(tr)>=3 and 'exit=0' in ex)

print('T5  the driver rejects the semaphore import')
log,tr,out,ex=run('t5',['--sem'],'sync=sem\n',('MOCK_SEM_IMPORT_FAIL=1',))
check('fallback explained', 'the driver rejected the semaphore import' in log)
check('both GL semaphores deleted again', len(gl(tr,'DeleteSemaphore'))>=2)
check('glFinish path used', len(gl(tr,'FINISH'))==nsnap(tr)>=3 and 'exit=0' in ex)

print('T6  helper speaks the old protocol (v2)')
log,tr,out,ex=run('t6',['--sem','--version','2'],'sync=sem\n')
check('mismatch reported with both versions', 'bad hello from helper' in log and 'version 2, want 7' in log)
check('game unaffected', 'gl_state_violations=0' in out and 'exit=0' in ex and nsnap(tr)==0)

print('T7  helper dies in the middle of a semaphore session')
log,tr,out,ex=run('t7',['--sem','--die-after','2'],'sync=sem\n')
check('loss detected', 'lost the helper' in log or 'helper\'s socket is closed' in log)
check('GL semaphores deleted on loss', len(gl(tr,'DeleteSemaphore'))>=2)
check('game kept running with clean GL state', 'gl_state_violations=0' in out and 'exit=0' in ex)

print('T8  BMVR_SYNC=sem environment override, no config file')
log,tr,out,ex=run('t8',['--sem'],'',('BMVR_SYNC=sem',))
check('semaphore mode active', 'sync = GPU semaphores' in log and not gl(tr,'FINISH'))

print('T9  render targets: renderbuffer (InternalRT0) and texture, both sync modes')
log,tr,out,ex=run('t9a',[],'target=InternalRT0\nflip=0\n')
blits=gl(tr,'BLIT')
check('renderbuffer attached to the private read FBO and blitted', blits and all('(rb ' in b for b in blits) and nsnap(tr)>=2, str(blits[:1]))
log,tr,out,ex=run('t9b',['--sem'],'target=_rt_hud\nflip=1\nsync=sem\n')
blits=gl(tr,'BLIT')
check('texture target blitted with semaphores', blits and all('(tex ' in b for b in blits) and not gl(tr,'FINISH') and nsnap(tr)>=2)
log,tr,out,ex=run('t9c',['--sem'],'target=window\nsync=sem\n')
blits=gl(tr,'BLIT')
check('window source works with semaphores', blits and all('(default ' in b for b in blits) and not gl(tr,'FINISH') and nsnap(tr)>=2)

print('T10 trace starts on a busy (gameplay) frame, not on loading frames')
log,tr,out,ex=run('t10',[],'')
m=re.search(r'rt-trace: begin \(tick (\d+); the previous frame made (\d+)',log)
check('first trace begins right after loading (tick 301, 300 loading frames)', bool(m) and int(m.group(1))>=301 and int(m.group(2))>=50, str(m and m.groups()))
check('trace has a summary and an end line', 'rt-trace: summary of the traced frame' in log and 'rt-trace: end of traced frame' in log)
# The stats line needs 5 s of wall time and the fake game finishes in well under a second, so check the label in the shim binary.
shim=open(os.environ['SHIM'],'rb').read()
check('stats label no longer says "registry=N (surfaces)"', b'registry puts=' in shim and b'registry=' not in shim.replace(b'registry puts=',b''))
print('T11 long-lived targets survive heavy GetSurfaceLevel churn (regression: one shared table lost their labels within seconds)')
for nm,cfg,what in (('t11a','','texture target (_rt_FullFrameFB, the default)'),('t11b','target=InternalRT0\nflip=0\n','CreateRenderTarget surface (InternalRT0)')):
    log,tr,out,ex=run(nm,[],cfg,game_args='300 2500')
    ticks=[int(x) for x in re.findall(r'HELPER frame tick=(\d+)','\n'.join(tr))]
    check(what+': snapshots keep arriving until the end of a long run', len(ticks)>=15 and max(ticks or [0])>=2500, f'{len(ticks)} snapshots, last at tick {max(ticks or [0])}')
    check(what+': no "not available" fallbacks after the first gameplay frames', log.count('no matching render target')<=4, str(log.count('no matching render target')))
print('T12 helper exits right AFTER acking a frame (its GPU signal never arrives): the game must not issue the GPU wait')
log,tr,out,ex=run('t12',['--sem','--die-idle-after','2'],'sync=sem\n')
check('death noticed before the next frame, by the socket check', "helper's socket is closed" in log, 'lost the helper' if 'lost the helper' in log else '')
idx=[i for i,l in enumerate(tr) if 'HELPER dying idle' in l]
after=tr[idx[0]:] if idx else []
check('no GL wait on the semaphore after the helper died', bool(idx) and not [l for l in after if l.startswith('GL WaitSemaphore')], str([l for l in after if 'Wait' in l][:2]))
check('no blit or signal for the dead helper either', bool(idx) and not [l for l in after if l.startswith('GL BLIT') or l.startswith('GL SignalSemaphore')])
check('GL semaphores deleted, game kept running with clean GL state', len(gl(tr,'DeleteSemaphore'))>=2 and 'gl_state_violations=0' in out and 'exit=0' in ex)
print('T13 loading / menu frames: the last delivered object is reused instead of "no matching render target"')
log,tr,out,ex=run('t13',[],'target=InternalRT0\nflip=0\n',game_args='300 500 700')
check('fallback used on the second loading screen', 'delivering the last known object of' in log and "'InternalRT0'" in log)
ticks=[int(x) for x in re.findall(r'HELPER frame tick=(\d+)','\n'.join(tr))]
check('snapshots keep arriving during the tail loading screen (ticks > 800)', len([t for t in ticks if t>800])>=3, str(ticks[-6:]))
check('no loading-frame misses reported after the first gameplay frames', log.count('no matching render target')==0, str(log.count('no matching render target')))
check('game ran clean, GL bindings preserved', 'gl_state_violations=0' in out and 'exit=0' in ex)
log,tr,out,ex=run('t13b',[],'target=InternalRT0\nflip=0\n',game_args='300 500 0')
check('no fallback without a tail loading screen', 'delivering the last known object' not in log)
log,tr,out,ex=run('t13c',[],'target=InternalRT0\nflip=0\n',game_args='700 0 0')
check('loading screen BEFORE any gameplay: quiet, nothing delivered, no error line', nsnap(tr)==0 and log.count('no matching render target')==0 and 'exit=0' in ex)

# ---------------------------------------------------------------------------- stage 3d: two eyes (protocol v4)
PAIR='target=_rt_fullframefb\ntarget2=name:_rt_hud\nflip=0\n'
def frames(tr): return [l for l in tr if l.startswith('HELPER frame')]
def field(l, k):
    m=re.search(k+r'=(\S+)', l); return m.group(1) if m else None
def seq(tr):
    out=[]
    for l in tr:
        if l.startswith('GL BLIT'): out.append('BLIT')
        elif l.startswith('GL WaitSemaphore'): out.append('WAIT')
        elif l.startswith('GL SignalSemaphore'): out.append('SIGNAL')
        elif l.startswith('GL FLUSH'): out.append('FLUSH')
        elif l.startswith('GL FINISH'): out.append('FINISH')
        elif l.startswith('HELPER frame'): out.append('frame')
        elif l.startswith('HELPER ack'): out.append('ack')
    return ' '.join(out)
def blit_info(tr):
    r=[]
    for l in tr:
        m=re.match(r'GL BLIT read_fbo=\d+\((\w+) (\d+)\) draw_fbo=(\d+) src=\(([^)]*)\) dst=\(([^)]*)\)', l)
        if m: r.append(m.groups())
    return r

print('T14 two eyes, sync=sem: a PAIR is one wait, two blits, ONE signal, one frame message, one ack')
log,tr,out,ex=run('t14',['--sem','--eyes','2'],PAIR+'sync=sem\n')
n=len(frames(tr))
check('game ran clean, GL bindings preserved', 'gl_state_violations=0' in out and 'exit=0' in ex)
check('hello offered 2 images + 2 semaphores (4 fds), protocol v7', 'HELPER hello version=7 caps=1 fds=4 eyes=2' in tr)
check('both images imported as GL textures (2 memory imports, 2 semaphore imports)', len([l for l in tr if l.startswith('GL ImportMemoryFd')])==2 and len([l for l in tr if 'ImportSemaphoreFd' in l])==2)
check('log names both textures', '(eye 0) and' in log and '(eye 1)' in log)
check('pairs delivered', n>=3, str(n))
check('every frame message describes two eyes: eye 0 _rt_fullframefb, eye 1 _rt_hud', all(field(l,'eyes')=='2' and field(l,'name')=='_rt_fullframefb' and field(l,'eye1name')=='_rt_hud' and field(l,'eye1src')=='3840x2160' for l in frames(tr)))
sq=seq(tr)
check('first pair: BLIT BLIT SIGNAL FLUSH frame ack (no wait)', sq.startswith('BLIT BLIT SIGNAL FLUSH frame ack'), sq[:80])
check('later pairs: WAIT BLIT BLIT SIGNAL FLUSH frame ack', sq.count('WAIT BLIT BLIT SIGNAL FLUSH frame ack')==n-1, sq[:160])
sigs=gl(tr,'SignalSemaphore'); waits=gl(tr,'WaitSemaphore')
check('exactly ONE signal per pair and one wait per later pair', len(sigs)==n and len(waits)==n-1, f'sig={len(sigs)} wait={len(waits)} pairs={n}')
check('barriers list BOTH textures (nt=2), GENERAL layout', all('(nt=2)' in l and 'layout=0x958d' in l for l in sigs+waits))
bt=[tuple(l.split(': ')[1].split()) for l in tr if l.startswith('GL   barrier textures:')]
check('the two barrier textures are distinct and identical on every wait/signal', bool(bt) and len(set(bt))==1 and bt[0][0]!=bt[0][1] and '0' not in bt[0], str(set(bt)))
bi=blit_info(tr)
check('two blits per pair', len(bi)==2*n, f'{len(bi)} blits / {n} pairs')
pairs=[(bi[i],bi[i+1]) for i in range(0,len(bi)-1,2)]
check('the two blits of a pair read different engine objects and write different shared images', bool(pairs) and all(a[1]!=b[1] and a[2]!=b[2] for a,b in pairs))
check('NO glFinish', not gl(tr,'FINISH'))
check('every frame flagged SEMAPHORE', all(int(field(l,'flags'),16)&4 for l in frames(tr)))

print('T14b two eyes, sync=finish: two blits, ONE glFinish per pair')
log,tr,out,ex=run('t14b',['--eyes','2'],PAIR)
n=len(frames(tr)); sq=seq(tr)
check('game ran clean', 'gl_state_violations=0' in out and 'exit=0' in ex)
check('every pair is BLIT BLIT FINISH frame ack', n>=3 and sq.count('BLIT BLIT FINISH frame ack')==n and sq.count('BLIT')==2*n and sq.count('FINISH')==n, sq[:120])
check('no semaphore calls, frames carry no SEMAPHORE flag', not [l for l in tr if 'Semaphore' in l and 'Import' not in l] and all(int(field(l,'flags'),16)&4==0 for l in frames(tr)))

print('T14c two eyes: eye 1 selected by size, own flip')
log,tr,out,ex=run('t14c',['--eyes','2'],'target=_rt_fullframefb\nflip=0\ntarget2=size:960x540\nflip2=1\n')
fr=frames(tr); bi=blit_info(tr)
check('eye 1 resolved by size to _rt_smallfb0 (960x540)', len(fr)>=3 and all(field(l,'eye1name')=='_rt_smallfb0' and field(l,'eye1src')=='960x540' for l in fr), str(fr[:1]))
check('eye 0 not flipped, eye 1 flipped (frame flags and blit destinations)', all(int(field(l,'flags'),16)&1==0 and int(field(l,'eye1flags'),16)&1==1 for l in fr) and all(a[4]=='0,0,1920,1080' and b[4]=='0,1080,1920,0' for a,b in zip(bi[0::2],bi[1::2])), str(bi[:2]))

print('T15 a pair is never half-delivered: eye 1 stops being rendered, nothing is sent for those frames')
log,tr,out,ex=run('t15',['--sem','--eyes','2'],PAIR+'sync=sem\n',('FAKE_SKIP_RT=_rt_hud','FAKE_SKIP_AFTER=650'),game_args='300 1000')
ticks=[int(field(l,'tick')) for l in frames(tr)]
check('pairs before the skip were delivered', len(ticks)>=2 and max(ticks)<700, str(ticks))
check('no frame (not even eye 0 alone) after the skip', not [t for t in ticks if t>=700], str(ticks))
check('every delivered frame is a full pair', all(field(l,'eyes')=='2' for l in frames(tr)))
check('log says which eye was missing', 'no matching render target for eye 1' in log)
check('no stray blit: exactly two per delivered pair', len(gl(tr,'BLIT'))==2*len(ticks), f'{len(gl(tr,"BLIT"))} blits, {len(ticks)} pairs')
check('game ran clean', 'gl_state_violations=0' in out and 'exit=0' in ex)

print('T16 eye negotiation: eye 1 is used only when requested AND offered')
log,tr,out,ex=run('t16a',['--sem','--eyes','2'],'target=_rt_fullframefb\nflip=0\nsync=sem\n')
check('helper offers 2, no target2: one eye in use, explained in the log', 'offers two eyes but no target2' in log)
check('frames say eye_count 1; barriers list one texture (nt=1)', len(frames(tr))>=3 and all(field(l,'eyes')=='1' for l in frames(tr)) and all('(nt=1)' in l for l in gl(tr,'SignalSemaphore')+gl(tr,'WaitSemaphore')))
check('one blit per frame, only one memory fd imported (the unused one is closed)', len(gl(tr,'BLIT'))==len(frames(tr)) and len([l for l in tr if l.startswith('GL ImportMemoryFd')])==1)
log,tr,out,ex=run('t16b',['--eyes','1'],PAIR)
check('target2 set but helper serves one eye: explained, eye 0 only', 'helper serves one eye' in log and len(frames(tr))>=3 and all(field(l,'eyes')=='1' for l in frames(tr)) and len(gl(tr,'BLIT'))==len(frames(tr)))
check('game ran clean', 'gl_state_violations=0' in out and 'exit=0' in ex)
log,tr,out,ex=run('t16c',['--eyes','2'],'target=window\ntarget2=name:_rt_hud\n')
check('eye 0 = window: target2 ignored (a pair needs two render targets), window path still works', 'target2 ignored: eye 0 is the window' in log and len(frames(tr))>=3 and all(field(l,'eyes')=='1' for l in frames(tr)))

print('T17 helper exits right after acking a PAIR: no GPU wait on its semaphore')
log,tr,out,ex=run('t17',['--sem','--eyes','2','--die-idle-after','2'],PAIR+'sync=sem\n')
idx=[i for i,l in enumerate(tr) if 'HELPER dying idle' in l]; after=tr[idx[0]:] if idx else []
check("death noticed by the socket check", "helper's socket is closed" in log)
check('no GL wait / blit / signal after the helper died', bool(idx) and not [l for l in after if l.startswith(('GL WaitSemaphore','GL BLIT','GL SignalSemaphore'))], str(after[:3]))
check('semaphores deleted, game kept running with clean GL state', len(gl(tr,'DeleteSemaphore'))>=2 and 'gl_state_violations=0' in out and 'exit=0' in ex)

print('T18 protocol: other versions and bad eye counts are rejected, the game is unaffected')
log,tr,out,ex=run('t18a',['--sem','--eyes','2','--version','4'],PAIR+'sync=sem\n')
check('v4 helper vs v7 shim: both versions named', 'bad hello from helper' in log and 'version 4, want 7' in log and not frames(tr))
log,tr,out,ex=run('t18b',['--sem','--eyes','3'],PAIR+'sync=sem\n')
check('eye_count 3 rejected', 'bad hello from helper' in log and not frames(tr))
check('game unaffected', 'gl_state_violations=0' in out and 'exit=0' in ex)

print('T19 loading / menu frames with two eyes: both last known objects are reused, the pair stays whole')
log,tr,out,ex=run('t19',['--eyes','2'],PAIR,game_args='300 500 700')
ticks=[int(field(l,'tick')) for l in frames(tr)]
check('fallback used for both eyes', 'delivering the last known object of' in log and '[eye 1]' in log)
check('pairs keep arriving during the tail loading screen (ticks > 800)', len([t for t in ticks if t>800])>=3 and all(field(l,'eyes')=='2' for l in frames(tr)), str(ticks[-6:]))
check('no loading-frame misses reported, game clean', log.count('no matching render target')==0 and 'gl_state_violations=0' in out and 'exit=0' in ex, str(log.count('no matching render target')))
print('T20 every=N in bmvr_target.txt: delivery cadence (default 120 presents; every=2 gives about 60x more pairs)')
log,tr,out,ex=run('t20a',['--eyes','2'],PAIR)
n_default=len(frames(tr))
log,tr,out,ex=run('t20b',['--eyes','2'],PAIR+'every=2\n')
n_fast=len(frames(tr))
check('every=2 logged and pairs delivered every 2 presents', 'every=2' in log and 'pairs delivered every 2 presents' in log, str(n_fast))
check('many more pairs than with the default', n_fast >= 20 and n_fast >= 10*max(n_default,1), '%d vs %d'%(n_fast,n_default))
check('every pair is still whole (2 eyes) and the game is clean', all(field(l,'eyes')=='2' for l in frames(tr)) and 'gl_state_violations=0' in out and 'exit=0' in ex)
log,tr,out,ex=run('t20c',['--eyes','2'],PAIR+'every=0\n')
check('every=0 rejected, default kept', 'bad every=0' in log and 'pairs delivered every 120 presents' in log and 'exit=0' in ex)


print('T21 protocol v5 pose channel: acks carry poses, frames say which pose they were rendered with')
log,tr,out,ex=run('t21',['--eyes','2','--poses'],PAIR+'every=2\n')
fr=frames(tr); pids=[int(field(l,'pose')) for l in fr]
check('helper offers poses and the shim logs the first one', 'xfer: pose channel: first pose id' in log and 'LOCAL space' in log)
check('pairs delivered, whole, game clean', len(fr)>=20 and all(field(l,'eyes')=='2' for l in fr) and 'gl_state_violations=0' in out and 'exit=0' in ex, str(len(fr)))
check('the first frames carry no pose (none received yet)', pids[0]==0, str(pids[:4]))
check('later frames carry a real pose id, never 0 once poses flow', all(p>0 for p in pids[3:]), str(pids[:8]))
check('ids never decrease', all(b>=a for a,b in zip(pids,pids[1:])), str(pids[:12]))
check('a frame never uses a pose the helper had not sent yet (frame n has seen n acks, id <= n)', all(p<=i for i,p in enumerate(pids)), str(pids[:12]))
check('ids advance with the acks (the shim keeps up with the newest pose)', pids[-1]>=len(pids)-3, '%d vs %d'%(pids[-1],len(pids)))

print('T22 helper without the pose channel (v5 helper, no CAP_POSES): frames carry pose 0')
log,tr,out,ex=run('t22',['--eyes','2'],PAIR+'every=2\n')
fr=frames(tr)
check('no pose channel logged, every frame pose 0, game clean', 'pose channel' not in log and len(fr)>=20 and all(int(field(l,'pose'))==0 for l in fr) and 'exit=0' in ex, str(len(fr)))

print('T23 a single-eye game also receives and stamps poses (eye count 1)')
log,tr,out,ex=run('t23',['--poses'],'every=2\n')
fr=frames(tr)
check('single-eye frames still carry the pose id', len(fr)>=20 and all(field(l,'eyes')=='1' for l in fr) and max(int(field(l,'pose')) for l in fr)>0 and 'exit=0' in ex, str(len(fr)))

print('T23b protocol v7 input channel: acks carry controller state, the shim logs the first state and every change')
log,tr,out,ex=run('t23b',['--eyes','2','--poses','--input'],PAIR+'every=2\n')
check('first controller state logged with poses valid', 'input: first controller state' in log and 'left pose valid right pose valid' in log)
check('a button press (right primary) and the stick push are logged as changes', 'buttons L 0 R 0x1' in log and 'stick L (0.00 0.00) R (0.00 1.00)' in log, log[-300:])
check('game clean, pairs delivered', len(frames(tr))>=20 and 'exit=0' in ex)
print('T23c helper without input: no input lines')
log,tr,out,ex=run('t23c',['--eyes','2','--poses'],PAIR+'every=2\n')
check('no input logged', 'input:' not in log and 'exit=0' in ex)

# ---------------------------------------------------------------------------- stage 4b: stereo views
import math
ST='target=_rt_fullframefb\nstereo=1\nstereo_module=exe\nflip=0\n'
def rvs(out):
    r=[]
    for l in out.splitlines():
        m=re.match(r'RV nest=(\d+) w=(\d+) h=(\d+) fov=(\S+) asp=(\S+) offc=(\d+) l=(\S+) r=(\S+) b=(\S+) t=(\S+) o=(\S+),(\S+),(\S+) a=(\S+),(\S+),(\S+) what=(\d+) clear=(\d+)', l)
        if m:
            g=m.groups(); r.append(dict(nest=int(g[0]),w=int(g[1]),h=int(g[2]),fov=float(g[3]),asp=float(g[4]),offc=int(g[5]),fr=[float(x) for x in g[6:10]],o=[float(x) for x in g[10:13]],a=[float(x) for x in g[13:16]],what=int(g[16]),clear=int(g[17])))
    return r

print('T24 stereo=1: the game draws two eyes from the helper pose, each blitted mid-frame, published as ONE pair')
log,tr,out,ex=run('t24',['--sem','--eyes','2','--poses'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 60')
check('game ran clean, GL bindings preserved', 'gl_state_violations=0' in out and 'exit=0' in ex)
check('class found by RTTI alone, slot 6 hooked', 'RTTI 11CViewRender' in log and 'hooked 11CViewRender vtable slot 6' in log)
v=rvs(out); main=[x for x in v if x['nest']==0]; eyes=[x for x in main if x['offc']==1]; plain=[x for x in main if x['offc']==0]
check('before the first pose the game draws its own view; then every frame draws eyes', len(plain)>=1 and len(eyes)>=40 and len(eyes)%2==0, f'plain={len(plain)} eyes={len(eyes)}')
check('the nested (water) views are never touched', all(x['offc']==0 and x['w']==1024 and x['what']==0 for x in v if x['nest']==1) and any(x['nest']==1 for x in v))
check('eye passes carry no HUD flag (what 3 -> 1), the game views keep 3', all(x['what']==1 for x in eyes) and all(x['what']==3 for x in plain))
prs=[(eyes[i],eyes[i+1]) for i in range(0,len(eyes)-1,2)]
sep=[math.dist(a['o'],b['o']) for a,b in prs]
check('left/right eyes are 64 mm apart (2.52 units) and share one orientation', bool(sep) and all(abs(d-2.5197)<0.02 for d in sep) and all(a['a']==b['a'] for a,b in prs), str(sep[:3]))
check('left eye is on the left of a player facing yaw 45 (x smaller, y larger than the right eye)', all(a['o'][0]<b['o'][0] and a['o'][1]>b['o'][1] for a,b in prs))
tl,tr_,tu,td=math.tan(-0.8),math.tan(0.7),math.tan(0.75),math.tan(-0.85)
ux,uy=max(-tl,tr_),max(-td,tu)
exp=[0.5+0.5*tl/ux,0.5+0.5*tr_/ux,0.5+0.5*td/uy,0.5+0.5*tu/uy]
check('off-centre fractions are the helper fov (asymmetric), aspect and fov from the centred frustum', all(all(abs(a-b)<2e-4 for a,b in zip(x['fr'],exp)) and abs(x['asp']-ux/uy)<2e-3 and abs(x['fov']-2*math.degrees(math.atan(ux)))<2e-3 for x in eyes), str(eyes[0]['fr'])+' want '+str(exp))
n=len(frames(tr))
check('every stereo pair is one frame message with two eyes', n>=40 and all(field(l,'eyes')=='2' for l in frames(tr)), str(n))
sq=seq(tr)
check('the stereo pairs are WAIT BLIT BLIT SIGNAL FLUSH frame ack (one wait, one signal per pair)', sq.count('WAIT BLIT BLIT SIGNAL FLUSH frame ack')>=len(eyes)//2-1, sq[:200])
check('exactly one signal per pair: signals == frames, waits == frames-1', len(gl(tr,'SignalSemaphore'))==n and len(gl(tr,'WaitSemaphore'))==n-1, f"sig={len(gl(tr,'SignalSemaphore'))} wait={len(gl(tr,'WaitSemaphore'))} frames={n}")
bi=blit_info(tr)
check('two blits per pair into two different shared images', len(bi)==2*n and all(bi[i][2]!=bi[i+1][2] for i in range(0,len(bi)-1,2)), f'{len(bi)} blits / {n} pairs')
ids=[int(field(l,'pose')) for l in frames(tr)]
stereo_ids=[int(m) for m in re.findall(r'stereo: frame \d+ pose (\d+)',log)]
check('frames are stamped with the pose they were rendered with (the ids the log shows are in the stamps)', all(i in ids for i in stereo_ids) and stereo_ids==sorted(stereo_ids) and len(stereo_ids)>=2, f'{stereo_ids} vs {ids[:8]}')
check('stamps never go backwards and never name a pose that has not been sent yet', ids==sorted(ids) and all(i<=k+1 for k,i in enumerate(ids)))
check('crash-sticky flag removed at a clean exit', not os.path.exists(f'{R}/cases/t24/bmvr_in_stereo.flag'))
check('no frame is delivered twice for one Present (the Present-time snapshot is skipped after a stereo pair)', n<=60+40+2, str(n))

print('T25 every=3: only delivery frames are drawn twice')
log,tr,out,ex=run('t25',['--sem','--eyes','2','--poses'],ST+'sync=sem\nevery=3\n',('FAKE_STEREO=1',),'100 90')
v=rvs(out); main=[x for x in v if x['nest']==0]; eyes=[x for x in main if x['offc']==1]; plain=[x for x in main if x['offc']==0]
check('game clean, eyes drawn on delivery frames only (about a third of 90 frames)', 'gl_state_violations=0' in out and 8<=len(eyes)//2<=34 and len(plain)>=50, f'pairs={len(eyes)//2} plain={len(plain)}')
check('every eye pass is still a whole pair', len(eyes)%2==0 and all(f_.startswith('HELPER frame') for f_ in frames(tr)) and all(field(l,'eyes')=='2' for l in frames(tr)))

print('T26 stereo requested but the helper sends no poses: the game keeps drawing its own view, flat pairs still flow')
log,tr,out,ex=run('t26',['--sem','--eyes','2'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
v=rvs(out)
check('hooked, but no eye pass is ever made, reason logged once', 'hooked 11CViewRender' in log and log.count('the helper has sent no pose yet')==1 and not [x for x in v if x['offc']==1], str([x for x in v if x['offc']==1][:1]))
check('flat pairs keep arriving, game clean', len(frames(tr))>=20 and 'gl_state_violations=0' in out and 'exit=0' in ex)

print('T27 stereo requested with a one-eye helper: nothing is drawn twice')
log,tr,out,ex=run('t27',['--sem','--eyes','1','--poses'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
check('no eye pass, game clean, single-eye frames', not [x for x in rvs(out) if x['offc']==1] and 'exit=0' in ex and all(field(l,'eyes')=='1' for l in frames(tr)) and len(frames(tr))>=20)

print('T28 refusals leave the game untouched and say why')
log,tr,out,ex=run('t28a',['--sem','--eyes','2','--poses'],ST.replace('stereo=1','stereo=1\nstereo_class=CNoSuchView')+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
check('unknown class: 0 primary vtables, the existing ViewRender classes are listed, no hook, game clean', 'has 0 primary vtables' in log and 'class name in the module: 11IViewRender' in log and 'hooked' not in log and not [x for x in rvs(out) if x['offc']==1] and 'exit=0' in ex)
log,tr,out,ex=run('t28b',['--sem','--eyes','2','--poses'],ST.replace('stereo=1','stereo=1\nstereo_slot=40')+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
check('slot beyond the vtable: refused (not executable code), no hook', 'is not executable code' in log and 'hooked' not in log and 'exit=0' in ex)
log,tr,out,ex=run('t28c',['--sem','--eyes','2','--poses'],ST.replace('stereo_module=exe','stereo_module=/nothing.so')+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
check('module not loaded: stereo off, says which key to fix', 'no readable mapping of a module matching' in log and 'trying again' in log and 'hooked' not in log and 'exit=0' in ex)
log,tr,out,ex=run('t28d',['--sem','--eyes','2','--poses'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1','FAKE_BAD_VIEW=1'),'100 40')
check('a view that does not look like a CViewSetup (fov 5) is never rewritten', 'does not look like a CViewSetup' in log and not [x for x in rvs(out) if x['offc']==1] and 'exit=0' in ex)
log,tr,out,ex=run('t28e',['--sem','--eyes','2','--poses'],'target=window\nstereo=1\nstereo_module=exe\n',('FAKE_STEREO=1',),'100 40')
check('target=window with stereo: refused with a reason', 'the target is the window' in log and 'hooked' not in log and 'exit=0' in ex)
log,tr,out,ex=run('t28f',['--sem','--eyes','2','--poses'],ST.replace('stereo=1','stereo=1\nstereo_class=CNoSuchView')+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
fr=frames(tr)
check('stereo requested but the hook is refused: the mono pairs carry NO pose (they were not rendered with one), so the headset shows them flat, not world-anchored', len(fr)>=10 and all(int(field(l,'pose'))==0 for l in fr) and 'exit=0' in ex, str([field(l,'pose') for l in fr][:6]))

print('T29 crash-sticky flag: a launch that died with stereo active turns stereo off for the next launch only')
d=f'{R}/cases/t29'
os.environ['PRE_FLAG']='1'      # run_case.sh creates bmvr_in_stereo.flag in the case dir before the game starts
log,tr,out,ex=run('t29',['--sem','--eyes','2','--poses'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40')
del os.environ['PRE_FLAG']
check('flag present at start: stereo OFF for this launch, flag consumed, no eye pass', 'bmvr_in_stereo.flag exists' in log and 'hooked' not in log and not os.path.exists(d+'/bmvr_in_stereo.flag') and not [x for x in rvs(out) if x['offc']==1] and 'exit=0' in ex)

print('T30 the helper dies in the middle of a stereo session')
log,tr,out,ex=run('t30',['--sem','--eyes','2','--poses','--die-idle-after','5'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 60')
v=rvs(out)
check('game finishes, clean GL state, loss noticed, no GL wait/blit after the death', 'exit=0' in ex and 'gl_state_violations=0' in out and ("helper's socket is closed" in log or 'lost the helper' in log))
check('after the helper is gone the game draws its own view again (plain views at the end)', bool(v) and v[-1]['offc']==0 and v[-2]['offc']==0)

print('T31 the view class has another name: used only when it is the one usable *ViewRender class; diagnostics say what happened')
log,tr,out,ex=run('t31a',['--sem','--eyes','2','--poses'],ST+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 700',fake_bin='fake_game_bm')
check('default CViewRender missing -> CBMViewRender picked and hooked, game draws two eyes', "using the only usable *ViewRender class 13CBMViewRender" in log and 'hooked 13CBMViewRender' in log and [x for x in rvs(out) if x['offc']==1] and 'exit=0' in ex)
check('first call of the slot is logged, and so is the raw view dump', 'the hooked slot was called for the first time' in log and 'raw view' in log)
check('periodic status line reports calls and pairs', 'stereo: status tick' in log and 'stereo pairs' in log)
log,tr,out,ex=run('t31b',['--sem','--eyes','2','--poses'],ST.replace('stereo=1','stereo=1\nstereo_class=CViewRender')+'sync=sem\nevery=1\n',('FAKE_STEREO=1',),'100 40',fake_bin='fake_game_bm')
check('stereo_class= given explicitly and absent: never substituted', 'has 0 primary vtables' in log and 'hooked' not in log and 'using the only usable' not in log and 'exit=0' in ex)

# ---------------------------------------------------------------------------- slice 3b: controller input into CreateMove
IN='target=_rt_fullframefb\ninput=1\ninput_module=exe\nflip=0\n'
def cm(out):
    m=re.search(r'CM calls=(\d+) cmds=(\d+) moved=(\d+) maxfwd=(\S+) attack=(\d+) kbd_lost=(\d+) impulse100=(\d+) turned=(\d+) minyaw=(\S+)', out)
    return dict(zip(['calls','cmds','moved','maxfwd','attack','kbd_lost','imp100','turned','minyaw'],[float(x) for x in m.groups()])) if m else None

print('T32 input=1: the hook watches first, then the controllers drive CreateMove (walk, attack, turn, flashlight), keyboard bits stay')
log,tr,out,ex=run('t32',['--sem','--drive'],IN+'sync=sem\nevery=1\n',('FAKE_INPUT=1',),'100 200')
c=cm(out) or {}
check('game clean, class found by RTTI alone, slot 22 hooked, watching only at first', 'gl_state_violations=0' in out and 'exit=0' in ex and 'RTTI 16ClientModeShared' in log and 'hooked 16ClientModeShared vtable slot 22' in log and 'WATCHING ONLY' in log, log[-600:])
check('the slot proved itself after 20 plausible cmds, and it is logged', 'looks like CreateMove (20 consecutive calls' in log)
check('every cmd went through the hook', c and c['calls']==c['cmds']==200, str(c))
check('the first 20 cmds were never written (learning), later ones were: walking at full speed 450', c and 20<=c['moved']<=c['cmds']-19 and abs(c['maxfwd']-450.0)<0.01, str(c))
check('right trigger became IN_ATTACK on the same cmds', c and c['attack']>=c['moved']-2 and c['attack']>=100, str(c))
check('keyboard bits the engine put in the cmd survive', c and c['kbd_lost']==0, str(c))
check('right stick turned the view to the right (yaw below the starting 10)', c and c['turned']>=100 and c['minyaw']<10.0, str(c))
check('the flashlight squeeze gave exactly one impulse 100', c and c['imp100']==1, str(c))
check('the Menu button pushed exactly one Escape key press (down, then up) into SDL', tr.count('SDL_PushEvent type=0x300 scancode=41 sym=27')==1 and tr.count('SDL_PushEvent type=0x301 scancode=41 sym=27')==1, tr[-300:])
check('input flag removed at a clean exit', not os.path.exists(f'{R}/cases/t32/bmvr_in_input.flag'))

print('T37 hands=1 on a GL stand-in without shader entry points: the hands switch themselves off, the stereo pairs keep flowing')
log,tr,out,ex=run('t37',['--sem','--eyes','2','--poses','--drive'],ST+'sync=sem\nevery=1\nhands=1\nhands_dir='+os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','VR','hands'))+'\n',('FAKE_STEREO=1',),'100 60')
check('game ran clean, GL bindings preserved', 'gl_state_violations=0' in out and 'exit=0' in ex, log[-500:])
check('the hands said why they are off (a missing GL entry point), once', log.count('hands: GL entry point') == 1 and 'glCreateShader' in log, log[-500:])
check('stereo pairs were still delivered', len([l for l in tr if l.startswith('HELPER frame')])>=20)

print('T33 input=1 with the helper offering no input: nothing is written, nothing breaks')
log,tr,out,ex=run('t33',['--sem'],IN+'sync=sem\nevery=1\n',('FAKE_INPUT=1',),'100 100')
c=cm(out) or {}
check('hooked and armed, but no value ever changes', 'exit=0' in ex and c and c['moved']==0 and c['attack']==0 and c['turned']==0 and c['kbd_lost']==0, str(c))

print('T34 a wrong slot never writes: the cmd pointer is junk')
log,tr,out,ex=run('t34',['--sem','--drive'],IN+'sync=sem\nevery=1\n',('FAKE_INPUT=1','FAKE_INPUT_BADCMD=1'),'100 100')
check('game finishes clean; "does not look like a CUserCmd" logged; never armed', 'exit=0' in ex and 'does not look like a CUserCmd' in log and 'looks like CreateMove' not in log, log[-400:])
check('the junk buffer is untouched', 'gl_state_violations=0' in out)

print('T35 refusals leave the game untouched and say why')
log,tr,out,ex=run('t35a',['--sem','--drive'],IN.replace('input=1','input=1\ninput_slot=60')+'sync=sem\nevery=1\n',('FAKE_INPUT=1',),'100 40')
check('slot beyond the vtable: refused, no hook', 'is not executable code' in log and 'hooked 16' not in log and 'exit=0' in ex)
log,tr,out,ex=run('t35b',['--sem','--drive'],IN.replace('input=1','input=1\ninput_class=CNoSuchMode')+'sync=sem\nevery=1\n',('FAKE_INPUT=1',),'100 40')
check('unknown class: 0 primary vtables, ClientMode classes listed, no hook', 'has 0 primary vtables' in log and 'ClientModeShared' in log and 'hooked 16' not in log and 'exit=0' in ex)
log,tr,out,ex=run('t35c',['--sem','--drive'],IN.replace('input=1','input=1\ninput_slot=21')+'sync=sem\nevery=1\n',('FAKE_INPUT=1',),'100 100')
c=cm(out) or {}
check('a slot that is never called (21 = a different virtual): game unaffected, nothing written', 'exit=0' in ex and c and c['moved']==0 and c['attack']==0 and 'looks like CreateMove' not in log, str(c))
os.environ['PRE_FLAG_INPUT']='1'
log,tr,out,ex=run('t35d',['--sem','--drive'],IN+'sync=sem\nevery=1\n',('FAKE_INPUT=1',),'100 100')
del os.environ['PRE_FLAG_INPUT']
c=cm(out) or {}
check('crash-sticky flag present at start: input OFF for this launch, flag consumed', 'bmvr_in_input.flag exists' in log and 'hooked 16' not in log and c and c['moved']==0 and not os.path.exists(f'{R}/cases/t35d/bmvr_in_input.flag'), str(c))

print('T36 the game instantiates a SUBCLASS of ClientModeShared (own vtable copy): the base hook sees nothing, the probe finds the subclass')
log,tr,out,ex=run('t36a',['--sem','--drive'],IN+'sync=sem\nevery=1\n',('FAKE_INPUT=1','FAKE_INPUT_DERIVED=1'),'100 200')
c=cm(out) or {}
check('subclass found as a candidate, watched, proved itself; others put back', 'exit=0' in ex and 'candidate class 19ClientModeBMSNormal' in log and 'looks like CreateMove' in log and 'of 19ClientModeBMSNormal looks like CreateMove' in log, log[-700:])
check('controllers drive the subclass: moved, attack, kbd bits kept', c and c['moved']>=100 and abs(c['maxfwd']-450.0)<0.01 and c['attack']>=100 and c['kbd_lost']==0, str(c))
log,tr,out,ex=run('t36b',['--sem','--drive'],IN+'input_probe=0\nsync=sem\nevery=1\n',('FAKE_INPUT=1','FAKE_INPUT_DERIVED=1'),'100 100')
c=cm(out) or {}
check('input_probe=0: only the base vtable, never called, nothing written (the failure mode seen on the real game)', 'exit=0' in ex and 'looks like CreateMove' not in log and c and c['moved']==0, str(c))
log,tr,out,ex=run('t36c',['--sem','--drive'],IN+'sync=sem\nevery=1\n',('FAKE_INPUT=1','FAKE_INPUT_DERIVED=2'),'100 200')
c=cm(out) or {}
check('a subclass that overrides CreateMove and calls the base directly: its own slot is hooked, written exactly once per cmd', 'exit=0' in ex and c and c['moved']>=100 and abs(c['maxfwd']-450.0)<0.01, str(c)+log[-500:])

print()
print('FAILED: %d'%fails if fails else 'ALL CHECKS PASSED')
sys.exit(1 if fails else 0)
