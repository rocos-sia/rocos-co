"""Real ROCOS-App integration. Only runs against a virtual CAN interface in an isolated network."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
p.add_argument('--app',type=Path,required=True)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--interface',default='vcan0')
p.add_argument('--test-shm-id',type=int,help='Test-only SHM ID >= 700000, shared across coordinated restarts')
p.add_argument('--scenario',choices=('normal','fault','wrong-id','wrong-name','wrong-width'),default='normal')
args=p.parse_args()
net=json.loads(subprocess.check_output(['ip','-j','-d','link','show','dev',args.interface]))[0]
if net.get('linkinfo', {}).get('info_kind') != 'vcan' or 'UP' not in net['flags']:
    raise SystemExit('Real-App integration requires an active vcan interface')
app=args.app.resolve()
build=args.build.resolve()
run=build/'app-integration'/f'{args.scenario}-{os.getpid()}'
shutil.copytree(ROOT/'tests/app/erob_6axis',run)
for child in ['scripts','web','tools']: (run/child).mkdir()
sid=args.test_shm_id or 700000+os.getpid()
if sid < 700000:
    raise SystemExit('Reserved test SHM ID must be at least 700000')
ap=run/'app.yaml'
ap.write_text(ap.read_text().replace('ecat_id: 2',f'ecat_id: {sid+(args.scenario=="wrong-id")}'))
if args.scenario=='wrong-name':
    f=run/'hardware.yaml'
    f.write_text(f.read_text().replace('position_actual_value: Position actual value','position_actual_value: wrong_name',1))
cfg=run/'co.yaml'
co=(ROOT/'config/erob_6axis/co.yaml').read_text().replace('interface: can0','interface: '+args.interface)
if args.scenario=='wrong-width':
    # Change the actual PDO description on the first node. A separate ABI probe checks the mismatch.
    co=co.replace('Position actual value, index: 0x6064, sub_index: 0, bit_length: 32',
                  'Position actual value, index: 0x6064, sub_index: 0, bit_length: 16',1)
cfg.write_text(co)
base='http://127.0.0.1:18080/api/'
headers={}
state_evidence={'scenario': args.scenario, 'master_id': sid}
def http(path,body=None):
    req=urllib.request.Request(base+path,data=None if body is None else json.dumps(body).encode(),
                               headers={'Content-Type':'application/json',**headers})
    with urllib.request.urlopen(req,timeout=2) as res:
        value=json.load(res)
    if not value.get('success'):raise RuntimeError(f'{path}: {value.get("code")} {value.get("message")}')
    return value['data']

def until(action,seconds=5):
    deadline=time.monotonic()+seconds
    error=None
    while time.monotonic()<deadline:
        try:
            result=action()
            if result:return result
        except (urllib.error.URLError,ConnectionError) as e:error=e
        time.sleep(.02)
    raise AssertionError(f'condition timed out: {error}')

procs=[]
def start(name,cmd):
    file=(run/(name+'.log')).open('w')
    proc=subprocess.Popen(cmd,cwd=run,stdout=file,stderr=subprocess.STDOUT)
    file.close()
    procs.append(proc)
    return proc
def stop(proc,timeout=4):
    if proc.poll() is not None:return proc.returncode
    proc.terminate()
    try:return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait(timeout=2)
def snap():
    result=subprocess.run([build/'bin/shm_client',str(sid),'snapshot'],capture_output=True,text=True,timeout=4)
    assert result.returncode==0,result.stderr
    lines=result.stdout.splitlines()
    return int(lines[0].split()[-1]),[list(map(int,r.split())) for r in lines[1:]]
def validate_positions(joints,raw):
    assert [j['name'] for j in joints]==[f'joint_{i}' for i in range(1,7)],joints
    assert all(abs(j['position']-v/100000)<.000015 for j,v in zip(joints,raw)),joints

try:
    assert subprocess.run([app,'--check_config','--config='+str(ap)],cwd=run,capture_output=True).returncode==0
    # The peer uses a deliberately short payload for the wrong-width deployment, matching its declared mapping.
    sim=start('nodes',[sys.executable,'-u',ROOT/'tests/vcan/six_nodes.py',args.interface,args.scenario,run/'frames.txt'])
    until(lambda:'READY' in (run/'nodes.log').read_text(),2)
    master=start('master',[build/'bin/rocos_co_master','--config',cfg,'--master-id',str(sid)])
    until(lambda:'[Ready]' in (run/'master.log').read_text())
    if args.scenario not in ('wrong-width',):
        instance,initial=snap()
        state_evidence.update(instance=instance,initial=initial)
    robot=start('app',[app,'--config='+str(ap)])
    if args.scenario=='wrong-id':
        state_evidence['app_exit'] = robot.wait(timeout=6)
        report = (run/'app.log').read_text()
        assert state_evidence['app_exit'] != 0 and 'v2 header' in report
        state_evidence['app_diagnostic'] = [line for line in report.splitlines() if 'v2 header' in line][-1:]
    elif args.scenario in ('wrong-name','wrong-width'):
        until(lambda: robot.poll() is not None or 'listening on' in (run/'app.log').read_text())
        report=(run/'app.log').read_text()
        key='wrong_name' if args.scenario=='wrong-name' else 'PDO size, mapping or write permission'
        if args.scenario=='wrong-width':
            check=subprocess.run([build/'bin/shm_client',str(sid),'audit-width'],text=True,capture_output=True,timeout=2)
            assert check.returncode==0 and 'actual=2 B' in check.stdout,check.stderr
            state_evidence['descriptor']=check.stdout.strip()
        assert key in report,report[-2000:]
        state_evidence['app_diagnostic']=[line for line in report.splitlines() if key in line][-3:]
    else:
        state=until(lambda:http('robot/state'))
        raw=[-1000*(i+1) for i in range(6)]
        validate_positions(state['joint_states'],raw)
        assert all(row[1:]==[v,v,8,8] for row,v in zip(initial,raw)),initial
        session=http('control/acquire',{'client_name':'vcan integration','client_id':'rocos-co-test'})
        headers.update({'X-Rocos-Client-Id':'rocos-co-test','X-Rocos-Control-Token':session['token']})
        http('robot/enable',{})
        until(lambda:http('robot/enabled')['enabled'])
        if args.scenario=='normal':
            target=[v+100*(i+1) for i,v in enumerate(raw)]
            http('robot/movej',dict(joints=[v/100000 for v in target],velocity=.1,acceleration=.4,jerk=2))
            def at_target():
                joints=http('robot/state')['joint_states']
                return joints if all(abs(j['position']-v/100000)<.000015 for j,v in zip(joints,target)) else None
            final=until(at_target,8)
            _,image=snap()
            validate_positions(final,target)
            assert all(abs(row[1]-v)<=1 and abs(row[2]-v)<=1 and row[3:]==[8,8] for row,v in zip(image,target)),image
            state_evidence.update(final_image=image,app_positions=[j['position'] for j in final])
            until(lambda:http('robot/state')['robot_state']=='STOPPED')
            http('robot/disable',{})
            until(lambda:http('robot/enabled')['disabled'])
            http('control/release',{})
            assert stop(robot)==0
            assert stop(master)==0
        else:
            # Match a deliberate runtime-only fault to the live App. The peer still emits heartbeats.
            (run/'inject-missing').touch()
            state_evidence['master_exit'] = master.wait(timeout=4)
            report = (run/'master.log').read_text()
            assert state_evidence['master_exit'] != 0 and 'TPDO missing' in report
            assert 'affected slot 4 node 5' in report and 'raw velocity unconfirmed' in report
            state_evidence['stop_observations'] = [line.strip() for line in report.splitlines() if 'stop slot' in line]
            assert len(state_evidence['stop_observations']) == 6
            state_evidence['app_exit_on_teardown']=stop(robot)
    stop(sim)
    if args.scenario=='normal':
        wire_cmd={}
        wire_feedback={}
        for line in (run/'frames.txt').read_text().splitlines():
            items=line.split()
            if len(items)!=4: continue
            _, direction, raw_id, payload=items
            cob=int(raw_id,16)
            if direction=='master' and 0x201<=cob<=0x206:
                cw,pos,mode=struct.unpack('<Hib',bytes.fromhex(payload))
                if cw&15==15 and mode==8: wire_cmd[cob-0x201]=pos
            if direction=='node' and 0x181<=cob<=0x186:
                _,pos=struct.unpack('<Hi',bytes.fromhex(payload))
                wire_feedback[cob-0x181]=pos
        for i,val in enumerate(target):
            assert abs(wire_cmd[i]-val)<=1 and abs(wire_feedback[i]-val)<=1,(wire_cmd,wire_feedback)
        state_evidence.update(wire_cmd=wire_cmd,wire_feedback=wire_feedback)
    (run/'result.json').write_text(json.dumps(state_evidence,indent=2)+'\n')
    print(f'{args.scenario} passed: {run}')
finally:
    for proc in reversed(procs):stop(proc)
