#!/usr/bin/env python3
"""Reconcile sequential compressed-file writes with H264 packet boundaries.
Requires one append-only filesink with no seek/rewrite; write return is not fsync.
"""
import argparse,bisect,importlib.util,json,statistics
from pathlib import Path
import av
p=argparse.ArgumentParser();p.add_argument('stream');p.add_argument('log');p.add_argument('--output',required=True);a=p.parse_args()
s=importlib.util.spec_from_file_location('camera',Path(__file__).with_name('review_camera.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
text=Path(a.log).read_text(errors='replace');writes=m.records(text,'SINK_WRITE');outputs=m.records(text,'CAM_OUTPUT');links=m.records(text,'CAM_LINK')
keys=('bytes','cumulative','start_ns','done_ns','duration_ns')
writes=[{**r,**{k:int(r[k]) for k in keys}} for r in writes]
packets=[]
with av.open(a.stream) as c:
 for packet in c.demux(video=0):
  if packet.size:packets.append(packet.size)
checks={};total=0;ordered=True
for i,r in enumerate(writes):
 total+=max(0,r['bytes'])
 ordered &= r['cumulative']==total and r['done_ns']>=r['start_ns'] and r['duration_ns']==r['done_ns']-r['start_ns']
 if i: ordered &= r['start_ns']>=writes[i-1]['done_ns']
checks['serial_write_accounting']=bool(writes) and ordered
checks['successful_writes']=bool(writes) and all(r['bytes']>0 for r in writes)
checks['single_fd']=len({r['fd'] for r in writes})==1
checks['write_bytes_equal_closed_file']=total==Path(a.stream).stat().st_size and bool(writes)
checks['demux_bytes_equal_file']=sum(packets)==Path(a.stream).stat().st_size
checks['packet_retrieval_count']=len(packets)==len(outputs)
header=packets[0]-int(outputs[0]['bytes']) if packets and outputs else None
checks['packet_size_association']=len(packets)==len(outputs) and all(n==int(r['bytes'])+(header if i==0 else 0) for i,(n,r) in enumerate(zip(packets,outputs)))
limits='Assumes one serial append-only sink without seeks/rewrites; exact packet sizes plus separate glyph validation establish association. Completion is successful write/writev return to the kernel, not fsync/durable storage or network delivery.'
frames=[];ends=[r['cumulative'] for r in writes];offset=0;linkby={int(r['input_n']):r for r in links}
if all(checks.values()):
 for i,(size,r) in enumerate(zip(packets,outputs)):
  offset+=size;j=bisect.bisect_left(ends,offset);w=writes[j];n=int(r['input_n']);link=linkby.get(n)
  frames.append({'input_n':n,'packet_end_byte':offset,'write_index':j,'write_done_ns':w['done_ns'],'retrieval_to_write_ms':(w['done_ns']-int(r['host_mono_ns']))/1e6,'dequeue_to_write_ms':(w['done_ns']-int(link['capture_host_ns']))/1e6 if link else None})
 checks['write_after_retrieval']=all(r['retrieval_to_write_ms']>=0 for r in frames)
# A byte-balance upper observation across retrieval/write-return events; this
# includes downstream copies, active write and filesink buffering, not queue level.
balance=0;peak=0;peak_ns=None;minimum=0
if all(checks.values()):
 events=[(int(o['host_mono_ns']),0,int(o['bytes'])+(header if i==0 else 0)) for i,o in enumerate(outputs)]
 events += [(w['done_ns'],1,-w['bytes']) for w in writes]
 for t,kind,delta in sorted(events):
  balance+=delta;minimum=min(minimum,balance)
  if balance>peak:peak=balance;peak_ns=t
 checks['retrieved_written_byte_balance']=balance==0 and minimum>=0
else:peak=None
backlog={'peak_bytes':peak,'peak_host_mono_ns':peak_ns,'final_bytes':balance,'minimum_bytes':minimum,'scope':'Retrieved compressed bytes plus first headers minus successful write-return bytes; includes downstream copies, filesink buffer and in-flight writes, not exact GstQueue occupancy.'}
def stats(v):
 v=[x for x in v if x is not None]
 return {'count':len(v),'min':min(v),'median':statistics.median(v),'max':max(v)} if v else None
r={'stream':a.stream,'checks':checks,'byte_mapping_pass':all(checks.values()),'write_calls':len(writes),'file_bytes':Path(a.stream).stat().st_size,'first_packet_header_bytes':header,'write_duration_ms':stats([w['duration_ns']/1e6 for w in writes]),'retrieval_to_write_ms':stats([f['retrieval_to_write_ms'] for f in frames]),'dequeue_to_write_ms':stats([f['dequeue_to_write_ms'] for f in frames]),'compressed_byte_backlog':backlog,'frames':frames,'limits':limits}
Path(a.output).write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({k:v for k,v in r.items() if k!='frames'},indent=2));raise SystemExit(0 if r['byte_mapping_pass'] else 2)
