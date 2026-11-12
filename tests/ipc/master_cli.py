"""Launch the deployable master, independent nodes and an ABI v2 client matched to the App."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time

master, client, simulator, sample, interface, state = sys.argv[1:]
shm_id=600000+os.getpid()
with tempfile.TemporaryDirectory() as tmp:
    directory=Path(tmp)
    source=Path(sample).read_text().replace('interface: can0','interface: '+interface)
    source=source.replace('state: op','state: '+('op' if state=='fault' else state))
    config=directory/'co.yaml'
    config.write_text(source)
    sim=subprocess.Popen([sys.executable,simulator,interface,'missing' if state=='fault' else 'mailbox',str(directory/'trace')],stdout=subprocess.PIPE,text=True)
    try:
        assert sim.stdout.readline()=='READY\n'
        args=[master,'--config',str(config),'--master-id',str(shm_id)]
        proc=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            for mode in ('monitor','duplicate',state):
                result=subprocess.run([client,str(shm_id),mode],capture_output=True,text=True,timeout=6)
                assert result.returncode==0,(mode,result.stdout,result.stderr)
            conflict=subprocess.run(args,capture_output=True,text=True,timeout=2)
            assert conflict.returncode == 3 and 'owner lock' in conflict.stderr,conflict.stderr
            if state!='fault': proc.terminate()
            out,err=proc.communicate(timeout=4)
            if state=='fault':
                assert proc.returncode == 4 and 'TPDO missing' in err and 'velocity unconfirmed' in err,(out,err)
            else:
                assert proc.returncode==0 and '[Ready]' in out and '[Shutdown]' in out and not err,(out,err)
            assert '[Startup]' in out and 'runtime license' in out and 'firmware eRob-simulation-1.9' in out
            assert '\x1b[' not in out and '\x1b[' not in err,err
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=4)
        assert not Path(f'/dev/shm/ecm_v2_{shm_id}').exists()
        # Failed open before ready must release the bus and both PDO owner locks.
        config.write_text(source.replace('interface: '+interface,'interface: absent_roc'))
        bad=subprocess.run(args,capture_output=True,text=True,timeout=2)
        assert bad.returncode == 3 and not any(Path('/dev/shm',p+str(shm_id)).exists() for p in ('ecm_v2_','pd_input_v2_','pd_output_v2_'))
    finally:
        sim.terminate()
        sim.wait(timeout=2)
