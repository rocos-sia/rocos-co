import bisect,csv,json,struct
from pathlib import Path
base=Path('build/bench-erob-3axis')
frames={};sync=[]
for line in (base/'run1-can.log').read_text().splitlines():
 t,_,frame=line.split();sec,frac=t[1:-1].split('.');stamp=int(sec)*10**9+int(frac.ljust(9,'0'))
 cob,payload=frame.split('#');cob=int(cob,16)
 frames.setdefault(cob,[]).append((stamp,bytes.fromhex(payload)))
 if cob==0x80:sync.append(stamp)
rows=list(csv.DictReader((base/'final-motion.csv').open()))
errors=[];lag=[]
for row in rows:
 stamp=int(row['timestamp_us'])*1000;node=int(row['slot'])+1
 for cob,fmt,names in [(0x180+node,'<Hi',['status','position']),(0x280+node,'<ib',['velocity','mode']),
                       (0x200+node,'<Hib',['control','target',None])]:
  vals=frames[cob];k=bisect.bisect_right(vals,(stamp,b'\xff'*8))-1
  ts,data=vals[k]
  actual=struct.unpack(fmt,data)
  expected=[int(row[name]) if name else 8 for name in names]
  if list(actual)!=expected:errors.append({'stamp':stamp,'cob':hex(cob),'actual':actual,'expected':expected})
  if cob<0x200:lag.append(stamp-ts)
start=int(rows[0]['timestamp_us'])*1000;end=int(rows[-1]['timestamp_us'])*1000
first=bisect.bisect_right(sync,start);last=bisect.bisect_right(sync,end)-1
out={'shm_rows':len(rows),'compared_pdo_frames':3*len(rows),'mismatch_count':len(errors),
     'first_mismatches':errors[:10],'max_tpdo1_to_shm_timestamp_ns':max(lag),
     'first_sync':first,'last_sync':last,'duration_s':(end-start)/1e9}
(base/'shm-can-audit.json').write_text(json.dumps(out,indent=2))
print(json.dumps(out,indent=2))
