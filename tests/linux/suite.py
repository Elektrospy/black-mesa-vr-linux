import subprocess, re, os, sys
R=os.environ['B']; RUN=os.path.join(os.path.dirname(os.path.abspath(__file__)),'run_case.sh')
fails=0
def run(name, helper, cfg, env=(), game_args='300 500'):
    subprocess.run([RUN, name, *helper, '--', cfg, '--', 'X=1', *env], check=True, env=dict(os.environ, GAME_ARGS=game_args))
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
check('mismatch reported with both versions', 'bad hello from helper' in log and 'version 2, want 4' in log)
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
check('hello offered 2 images + 2 semaphores (4 fds), protocol v4', 'HELPER hello version=4 caps=1 fds=4 eyes=2' in tr)
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
log,tr,out,ex=run('t18a',['--sem','--eyes','2','--version','3'],PAIR+'sync=sem\n')
check('v3 helper vs v4 shim: both versions named', 'bad hello from helper' in log and 'version 3, want 4' in log and not frames(tr))
log,tr,out,ex=run('t18b',['--sem','--eyes','3'],PAIR+'sync=sem\n')
check('eye_count 3 rejected', 'bad hello from helper' in log and not frames(tr))
check('game unaffected', 'gl_state_violations=0' in out and 'exit=0' in ex)

print('T19 loading / menu frames with two eyes: both last known objects are reused, the pair stays whole')
log,tr,out,ex=run('t19',['--eyes','2'],PAIR,game_args='300 500 700')
ticks=[int(field(l,'tick')) for l in frames(tr)]
check('fallback used for both eyes', 'delivering the last known object of' in log and '[eye 1]' in log)
check('pairs keep arriving during the tail loading screen (ticks > 800)', len([t for t in ticks if t>800])>=3 and all(field(l,'eyes')=='2' for l in frames(tr)), str(ticks[-6:]))
check('no loading-frame misses reported, game clean', log.count('no matching render target')==0 and 'gl_state_violations=0' in out and 'exit=0' in ex, str(log.count('no matching render target')))
print()
print('FAILED: %d'%fails if fails else 'ALL CHECKS PASSED')
sys.exit(1 if fails else 0)
