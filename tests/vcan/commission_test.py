"""Exercise six-node startup, failure gates and one-SYNC-cycle feedback/targets using a separate process."""
from pathlib import Path
import subprocess
import sys

exe, original, interface, scenario, work = sys.argv[1:]
work=Path(work)/scenario
work.mkdir(parents=True,exist_ok=True)
config=Path(original).read_text().replace('interface: can0','interface: '+interface).replace('duration: 0','duration: 260')
if scenario.startswith('verify'):
    config=config.replace('pdo_policy: configure','pdo_policy: verify')
if scenario=='preop':
    config=config.replace('state: op','state: preop')
cfg=work/'co.yaml'
cfg.write_text(config)
trace=work/'frames.txt'
sim=subprocess.Popen([sys.executable,str(Path(__file__).with_name('six_nodes.py')),interface,scenario,str(trace)],
                     stdout=subprocess.PIPE,text=True)
try:
    assert sim.stdout.readline()=='READY\n'
    proc=subprocess.run([exe,str(cfg)],text=True,capture_output=True,timeout=13)
finally:
    sim.terminate()
    sim.wait(timeout=2)
report=proc.stdout+proc.stderr
(work/'master.log').write_text(report)
print(report)
startup_error={'bad-id':'1018:2','partial-abort':'SDO abort 0x6040041','verify-bad':'1a01:2'}
rows=[line.split() for line in trace.read_text().splitlines()]
if scenario in startup_error:
    assert proc.returncode and startup_error[scenario] in report and 'READY' not in report,report
    assert not any(r[1]=='master' and (r[2]=='080' or (0x201<=int(r[2],16)<=0x206)) for r in rows)
elif scenario=='preop':
    assert proc.returncode==0 and 'READY preop' in report
    assert not any(r[1]=='master' and r[2]=='080' for r in rows)
else:
    assert 'READY op' in report and 'targets 0 ' not in report,report
    faults={'missing':'TPDO missing','dlc':'feedback window','emcy':'node EMCY','reboot':'NMT state','busoff':'bus-off'}
    if scenario in faults:
        assert proc.returncode and faults[scenario] in report, report
        if scenario in ('missing','dlc'):
            assert 'published 23' in report and 'velocity_mask 47' in report, report
    else:
        assert proc.returncode==0,report
    windows=[]
    for r in rows:
        if r[1]=='master' and r[2]=='080': windows.append([])
        elif windows: windows[-1].append(r)
    valid=0
    stopped=False
    for window in windows:
        commands=[r for r in window if r[1]=='master' and 0x201<=int(r[2],16)<=0x206]
        normal=[r for r in commands if r[3].startswith('0f00')]
        quick=[r for r in commands if r[3].startswith('0200')]
        if normal:
            assert not stopped
            assert {r[2] for r in normal}=={f'{0x201+i:03x}' for i in range(6)}
            feedback=[r for r in window if r[1]=='node' and 0x181<=int(r[2],16)<=0x286]
            assert len(feedback)==12 and all(int(r[0])<int(normal[0][0]) for r in feedback)
            valid+=1
        stopped |= bool(quick)
    assert valid>=1 and stopped==(scenario!="busoff")
