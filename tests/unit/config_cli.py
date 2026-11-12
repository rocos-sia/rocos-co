"""Exercise the installed CLI contract with generated inputs, without a CAN interface."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe, sample = sys.argv[1:]

def run(args, error=None, contains=None):
    proc = subprocess.run([exe, *args], capture_output=True, text=True, timeout=5)
    if error is None:
        assert proc.returncode == 0, (proc.stderr, args)
        assert not contains or contains in proc.stdout, proc.stdout
    else:
        assert proc.returncode != 0 and error in proc.stderr, (proc.returncode, proc.stderr, error)

def slave(i):
    return dict(id=i, node_id=i+1, name=f'axis{i}', identity=dict(vendor_id=1, product_code=2),
                rx_pdos=[], tx_pdos=[])

def pdos(count, feedback):
    frames = []
    for i in range(count):
        offset = i // 8
        if i % 8 == 0:
            frames.append(dict(index=(0x1a00 if feedback else 0x1600)+offset,
                               cob_id=0x201+offset, transmission_type=1, entries=[]))
        frames[-1]['entries'].append(dict(name=f'v{i}',index=0x2000+i,sub_index=0,bit_length=8))
    return frames

with tempfile.TemporaryDirectory() as td:
    path=Path(td)/'input.yaml'
    args=['--config', str(path), '--check-config']
    base=dict(version=1,master=dict(interface='absent_can',bitrate=1000000,node_id=127),slaves=[slave(0)])

    def check(config, error=None, contains=None):
        path.write_text(json.dumps(config,indent=2))
        run(args,error,contains)

    for size in (50,51):
        check(dict(base,slaves=[slave(i) for i in range(size)]), '$.slaves' if size==51 else None)
    for direction in ('rx_pdos','tx_pdos'):
        for count in (0,25,26):
            s=slave(0)
            s[direction]=pdos(count,direction=='tx_pdos')
            check(dict(base,slaves=[s]), 'SHM variable capacity' if count==26 else None)
    for key, val, err in [('cycle',-1,'$.master.cycle'),('state','safeop','$.master.state'),
                          ('eni',1,'unknown key'),('cpuidx',9999,'$.master.cpuidx'),
                          ('command_timeout_ms',60001,'$.master.command_timeout_ms')]:
        check(dict(base,master=dict(base['master'],**{key:val})),err)
    for s,err in [(dict(slave(1),node_id=1),'duplicate node ID'),(dict(slave(1),node_id=127),'duplicate node ID'),
                  (dict(slave(1),id=2),'slaves[1].id'),(dict(slave(1),name='长'*27),'SHM name too long')]:
        check(dict(base,slaves=[slave(0),s]),err)
    example=Path(sample).read_text()
    for old,new,err in [
        ('cob_id: 0x202','cob_id: 0x201','duplicate COB-ID'),
        ('cob_id: 0x201','cob_id: 0x581','reserved'),
        ('cob_id: 0x201','cob_id: 0x080','reserved'),
        ('bit_length: 32','bit_length: 17','bit_length (line'),
        ('bit_length: 32','bit_length: 48','bit_length (line'),
        ('Target Position','Control word','duplicate or oversized'),
        ('transmission_type: 1','transmission_type: 255','transmission_type'),
        ('cycle: 8000','cycle: 1000','insufficient bus budget'),
        ('bitrate: 1000000','bitrate: 500000','bitrate (line'),
        ('version: 1','version: 1\nversion: 2','duplicate key'),
        ('heartbeat_timeout_ms: 300','heartbeat_timeout_ms: 101','heartbeat_timeout_ms'),
        ('state: op','state: op\n  eni: ignored','unknown key'),
        ('index: 0x1A01','index: 0x1A00','duplicate mapping object'),
        ('version: 1','version: 2','$.version'),
    ]:
        assert old in example
        path.write_text(example.replace(old,new,1))
        run(args,err)
    for direction,limit in [('rx_pdos',6),('tx_pdos',12)]:
        nodes=[slave(i) for i in range(limit+1)]
        for i,s in enumerate(nodes):
            s[direction]=pdos(1,direction=='tx_pdos')
            s[direction][0]['cob_id']+=i
        check(dict(base,slaves=nodes),'local OD')
    cmd=dict(name='home',index=0x607c,sub_index=0,Type='int32',Value=-17)
    s=dict(slave(0),identity=dict(vendor_id=0x5a65726f,product_code=0x26483052),init_commands=[cmd])
    check(dict(base,slaves=[s]))
    for field,val,err in [('Value',2**31,'Value (line'),('index',0x6040,'allowlist'),('Type','uint32','requires int32')]:
        bad=copy.deepcopy(s)
        bad['init_commands'][0][field]=val
        check(dict(base,slaves=[bad]),err)
    run(['--config',sample,'--check-config','--period-us','1000'],'insufficient bus budget')
    run(['--config',sample,'--check-config','--master-id','5','--cpu',str(min(os.sched_getaffinity(0)))],contains='SHM ID 5 (CLI)')
    for args,err in [([], '--config'),(['--config'],'requires a value'),(['--foo'],'unknown'),
                     (['--master-id','2abc'],'expected decimal'),(['--cpu','1','--cpu','2'],'duplicate')]:
        run(args,err)
