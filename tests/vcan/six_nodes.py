"""Six independent CANopen node states on one virtual bus, using Python's native SocketCAN API."""
import argparse
from pathlib import Path
import select
import signal
import socket
import struct
import time

CAN = struct.Struct("=IB3x8s")
RX = (0x60400010, 0x607a0020, 0x60600008)
TX1 = (0x60410010, 0x60640020)
TX2 = (0x606c0020, 0x60610008)


class Drive:
    def __init__(self, node, scenario):
        self.node = node
        self.state = 5 if scenario == 'warm' else 127
        self.position = -1000*node
        self.status = 0x40
        self.mode = 8
        self.pending = None
        self.next_hb = 0
        self.objects = {}
        self.upload = None
        self.download = None
        self.sync_count = 0
        self.last_write = None
        for idx, sub, val, size in [
            (0x1018,0,3,1), (0x1018,1,0x5a65726f,4), (0x1018,2,0x26483052,4),
            (0x1018,3,0x20111,4), (0x1005,0,0x80,4), (0x1016,1,0,4),
            (0x1017,0,20 if scenario in ('warm','boot') else 0,2),
            (0x607c,0,0,4), (0x6085,0,200,4),
        ]:
            self.store(idx,sub,val,size)
        self.objects[0x1001,0]=bytes(1)
        self.objects[0x1008,0]=bytes(range(64))
        self.objects[0x1009,0]=b'test'
        self.objects[0x6084,0]=bytes(4)
        self.objects[0x100a,0] = b'eRob-simulation-1.9'
        for base, cob in [(0x1400,0x200),(0x1800,0x180)]:
            for i in range(4):
                self.store(base+i,1,(0 if i==0 else 0x80000000)+cob+i*0x100+node,4)
                self.store(base+i,2,255,1)
                self.store(base+0x200+i,0,1 if i==0 else 0,1)
                for j in range(1,9):
                    self.store(base+0x200+i,j,0,4)
        if scenario in ('verify','verify-bad'):
            for comm, cob, entries in [(0x1400,0x200,RX),(0x1800,0x180,TX1),(0x1801,0x280,TX2)]:
                self.store(comm,1,cob+node,4)
                self.store(comm,2,1,1)
                self.store(comm+0x200,0,len(entries),1)
                for j, entry in enumerate(entries,1):
                    self.store(comm+0x200,j,entry,4)
        if node==4 and scenario=='bad-id':
            self.store(0x1018,2,99,4)
        if node==4 and scenario=='verify-bad':
            self.store(0x1a01,2,0x60610010,4)

    def store(self, idx, sub, val, size):
        self.objects[idx,sub]=val.to_bytes(size,'little')

    def value(self, idx, sub):
        return int.from_bytes(self.objects[idx,sub],'little')

    def sdo(self, data, scenario):
        cs = data[0]
        if cs==0x80:
            self.upload=self.download=None
            return
        if cs&0xe0==0x60 and self.upload is not None:
            segment, self.upload = self.upload[:7],self.upload[7:]
            last=not self.upload
            if last: self.upload=None
            return bytes([(cs&16)|((7-len(segment))<<1)|last])+segment.ljust(7,b'\0')
        idx=int.from_bytes(data[1:3],'little')
        sub=data[3]
        key=idx,sub
        head=data[1:4]
        def abort(code):
            return b'\x80'+head+code.to_bytes(4,'little')
        if key not in self.objects:
            return abort(0x06020000)
        if cs==0x40:
            val=self.objects[key]
            if len(val)<=4:
                return bytes([0x43+4*(4-len(val))])+head+val.ljust(4,b'\0')
            self.upload=val
            return b'\x41'+head+len(val).to_bytes(4,'little')
        if cs in (0x2f,0x2b,0x27,0x23):
            if scenario=='partial-abort' and self.node==3 and key==(0x1a01,2):
                return abort(0x06040041)
            if self.state!=127:
                return abort(0x08000022)
            size=4-((cs>>2)&3)
            if len(self.objects[key])!=size:
                return abort(0x06070010)
            if (0x1600<=idx<0x1604 or 0x1a00<=idx<0x1a04):
                if not self.value(idx-0x200,1)&0x80000000 or (sub and self.value(idx,0)):
                    return abort(0x06040041)
            self.objects[key]=data[4:4+size]
            self.last_write=key
            if scenario=='mailbox' and key==(0x6084,0): return None
            return b'\x60'+head+bytes(4)
        return abort(0x05040001)

    def on_sync(self, send, scenario):
        if self.state!=5:
            return
        self.sync_count+=1
        if self.node==5 and self.sync_count==24:
            if scenario=='emcy': send(0x80+self.node,b'\x10\x81'+bytes(6))
            if scenario=='reboot': send(0x700+self.node,b'\0')
            if scenario=='busoff': send(socket.CAN_ERR_FLAG|0x40,bytes(8))
        if self.pending is not None:
            cw,pos,mode=struct.unpack('<Hib',self.pending)
            self.pending=None
            self.mode=mode
            if cw==2:
                self.status=0x07
            elif cw&0x8f==6: self.status=0x21
            elif cw&0x8f==7: self.status=0x23
            elif cw&0x8f==0: self.status=0x40
            elif cw&0xf==0xf:
                self.status=0x27
                self.position=pos
        for comm, pack in [(0x1800,struct.pack('<Hi',self.status,self.position)),
                           (0x1801,struct.pack('<ib',0,self.mode))]:
            cob=self.value(comm,1)
            if cob&0x80000000 or self.value(comm,2)!=1: continue
            inject=self.node==5 and self.sync_count>=24 and comm==0x1801
            if inject and scenario=='missing': continue
            if scenario=='wrong-width' and self.node==1 and comm==0x1800: pack=pack[:4]
            send(cob,pack[:-1] if inject and scenario=='dlc' else pack)


def main():
    p=argparse.ArgumentParser()
    p.add_argument('interface')
    p.add_argument('scenario')
    p.add_argument('trace',type=Path)
    args=p.parse_args()
    drives={n:Drive(n,args.scenario) for n in range(1,7)}
    running=True
    def stop(*_):
        nonlocal running
        running=False
    signal.signal(signal.SIGTERM,stop)
    with socket.socket(socket.PF_CAN,socket.SOCK_RAW,socket.CAN_RAW) as bus, args.trace.open('w') as log:
        bus.bind((args.interface,))
        def record(direction,cob,data):
            log.write(f'{time.monotonic_ns()} {direction} {cob:03x} {data.hex()}\n')
        def send(cob,data):
            bus.send(CAN.pack(cob,len(data),data.ljust(8,b'\0')))
            record('node',cob,data)
        print('READY',flush=True)
        if args.scenario=='boot':
            for d in drives.values(): send(0x700+d.node,b'\0')
        while running:
            now=time.monotonic()
            for d in drives.values():
                hb=d.value(0x1017,0)/1000
                if hb and now>=d.next_hb:
                    send(0x700+d.node,bytes([d.state]))
                    d.next_hb=now+hb
            if not select.select([bus],[],[],0.001)[0]: continue
            cob,size,raw=CAN.unpack(bus.recv(16))
            data=raw[:size]
            record('master',cob,data)
            if cob==0 and len(data)==2 and data[1] in drives:
                d=drives[data[1]]
                d.state={0x80:127,1:5,2:4}.get(data[0],d.state)
                d.next_hb=0
            elif cob==0x80:
                fault=args.scenario=='fault' and (args.trace.parent/'inject-missing').exists()
                for d in drives.values(): d.on_sync(send,'missing' if fault else args.scenario)
            elif 0x601<=cob<=0x606:
                d=drives[cob-0x600]
                response=d.sdo(data,args.scenario)
                if args.scenario=='mailbox' and data[:4]==b'\x40\x09\x10\x00': time.sleep(0.02)
                if response: send(0x580+d.node,response)
            elif 0x201<=cob<=0x206 and len(data)==7:
                d=drives[cob-0x200]
                if d.state==5 and d.value(0x1400,1)==cob and d.value(0x1400,2)==1: d.pending=data
if __name__=='__main__':
    main()
