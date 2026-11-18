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
check('mismatch reported with both versions', 'bad hello from helper' in log and 'version 2, want 3' in log)
check('game unaffected', 'gl_state_violations=0' in out and 'exit=0' in ex and nsnap(tr)==0)

print('T7  helper dies in the middle of a semaphore session')
log,tr,out,ex=run('t7',['--sem','--die-after','2'],'sync=sem\n')
check('loss detected', 'lost the helper' in log)
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
check('stats label no longer says "registry=N (surfaces)"', 'registry puts=' in log and 'registry=' not in re.sub('registry puts=','',log))
print('T11 long-lived targets survive heavy GetSurfaceLevel churn (regression: one shared table lost their labels within seconds)')
for nm,cfg,what in (('t11a','','texture target (_rt_FullFrameFB, the default)'),('t11b','target=InternalRT0\nflip=0\n','CreateRenderTarget surface (InternalRT0)')):
    log,tr,out,ex=run(nm,[],cfg,game_args='300 2500')
    ticks=[int(x) for x in re.findall(r'HELPER frame tick=(\d+)','\n'.join(tr))]
    check(what+': snapshots keep arriving until the end of a long run', len(ticks)>=15 and max(ticks or [0])>=2500, f'{len(ticks)} snapshots, last at tick {max(ticks or [0])}')
    check(what+': no "not available" fallbacks after the first gameplay frames', log.count('no matching render target')<=4, str(log.count('no matching render target')))
print()
print('FAILED: %d'%fails if fails else 'ALL CHECKS PASSED')
sys.exit(1 if fails else 0)
