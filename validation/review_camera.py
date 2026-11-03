#!/usr/bin/env python3
"""Evidence-only companion to validate_dynamic.py; no target access or decoding."""
import argparse,json,re,statistics
from pathlib import Path

def records(text, marker):
    # Limit each record to its own line/next recognized marker; tolerate vendor prefixes.
    found=[]
    for m in re.finditer(re.escape(marker)+r'\s+([^\n]*)',text):
        body=re.split(r'\b(?:BENCH |TS_FRAME |CAM_CAPTURE |CAM_LINK |CAM_OUTPUT )',m.group(1))[0]
        found.append(dict(re.findall(r'(\w+)=([^\s]+)',body)))
    return found

def integer(r,k): return int(r[k],0)
def cadence(rows,key,target):
    t=[float(r[key]) for r in rows]
    if len(t)<2:return {'available':False}
    gaps=[b-a for a,b in zip(t,t[1:])]
    rate=(len(t)-1)/(t[-1]-t[0]) if t[-1]>t[0] else 0
    windows=[]
    step=max(1,round(target*10))
    for i in range(0,len(t)-step,step):
        dt=t[i+step]-t[i]
        windows.append(step/dt if dt>0 else 0)
    return {'available':True,'samples':len(t),'duration_s':t[-1]-t[0], 'fps':rate,
      'strictly_increasing':all(g>0 for g in gaps),'max_interval_ms':max(gaps)*1000,
      'intervals_over_1_5_period':sum(g>1.5/target for g in gaps),'ten_second_block_fps':windows}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log');p.add_argument('glyph_validation');p.add_argument('--output',required=True)
    p.add_argument('--fps',type=float,default=60);p.add_argument('--min-seconds',type=float,default=180)
    p.add_argument('--warmup-seconds',type=float,default=1.0,help='Fixed predeclared source-time warmup for separate steady review')
    p.add_argument('--rate-tolerance',type=float,default=.01);p.add_argument('--allow-final-eos-loss',action='store_true')
    a=p.parse_args();text=Path(a.log).read_text(errors='replace');v=json.loads(Path(a.glyph_validation).read_text())
    issues=[];checks={}
    def check(name,value,reason):
        checks[name]=bool(value)
        if not value:issues.append(reason)
    ts=records(text,'TS_FRAME');enc=records(text,'BENCH ENCODE');inp=records(text,'BENCH INPUT');used=records(text,'BENCH USED')
    try:
        trip=lambda rs:[(integer(r,'id'),integer(r,'pts')) for r in rs]
        check('trace_order',bool(inp) and trip(inp)==trip(enc)==trip(used),'Missing or mismatched INPUT/ENCODE/USED trace')
        ens=[integer(r,'n') for r in enc];seq=[integer(r,'seq') for r in ts]
        check('explicit_timestamp_trace_mapping',bool(ts) and len(ts)==len(enc) and len(set(ens))==len(ens) and seq==ens and all(integer(t,'pts')==integer(e,'pts') and integer(t,'rc')==integer(e,'rc')==0 for t,e in zip(ts,enc)), 'TS_FRAME sequence/PTS/results do not map exactly to ENCODE n/PTS/results')
    except (KeyError,ValueError):check('trace_order',False,'Malformed trace fields');check('explicit_timestamp_trace_mapping',False,'Missing explicit ENCODE n or timestamp fields')
    check('glyph_association',v.get('dynamic_association_pass') is True,'Decoded glyph association failed or absent')
    expected=[integer(t,'seq') for t in ts if t.get('rc')=='0']
    visible=[f.get('sequence') for f in v.get('frames',[]) if f.get('recognized')]
    missing=sorted(set(expected)-set(visible));allowed=[expected[-1]] if expected and a.allow_final_eos_loss else []
    exact=bool(expected) and len(expected)==len(set(expected)) and (visible==expected or (a.allow_final_eos_loss and visible==expected[:-1]))
    check('exact_output_accounting',exact,'Output must be full input sequence, or exactly final one missing when explicitly allowed')
    captures=records(text,'CAM_CAPTURE');links=records(text,'CAM_LINK');outputs=records(text,'CAM_OUTPUT')
    driver=host=completion={'available':False}
    pts_offset_review={'available':False}
    try:
        required=('source_seq','driver_s','host_s','flags','bytesused','error','index')
        check('capture_fields',bool(captures) and all(all(k in r for k in required) for r in captures),'CAM_CAPTURE source sequence, driver/host timestamps, flags, bytesused/error/index evidence missing')
        if checks['capture_fields']:
            sq=[integer(r,'source_seq') for r in captures]
            check('capture_continuity',all(b==a+1 for a,b in zip(sq,sq[1:])) and len(sq)==len(set(sq)),'Hardware capture sequence gaps/repeats')
            check('capture_integrity_flags',all(integer(r,'error')==0 and integer(r,'bytesused')>0 and not(integer(r,'flags')&64) for r in captures),'Capture error/empty buffer flags')
            driver=cadence(captures,'driver_s',a.fps);host=cadence(captures,'host_s',a.fps)
            for name,c in [('driver',driver),('host',host)]:
                check(name+'_cadence',c['available'] and c.get('strictly_increasing',False) and abs(c.get('fps',0)/a.fps-1)<=a.rate_tolerance,'Actual '+name+' capture cadence outside tolerance')
                check(name+'_duration',c.get('duration_s',0)+1/a.fps>=a.min_seconds,'Insufficient '+name+' capture duration')
            byseq={integer(r,'source_seq'):r for r in captures}
            check('source_input_mapping',bool(links) and [integer(r,'input_n') for r in links]==[integer(r,'n') for r in inp] and len({integer(r,'source_seq') for r in links})==len(links) and all(integer(r,'source_seq') in byseq and integer(r,'input_id')==integer(i,'id') and integer(r,'pts_us')==integer(i,'pts') for r,i in zip(links,inp)), 'Missing/nonunique capture-to-input mapping (CAM_LINK)')
            if checks['source_input_mapping']:
                linked_seq=[integer(r,'source_seq') for r in links]
                check('linked_source_continuity',all(y==x+1 for x,y in zip(linked_seq,linked_seq[1:])), 'Captured frames skipped between encoder inputs')
                offsets=[integer(r,'pts_us')/1e6-float(byseq[integer(r,'source_seq')]['driver_s']) for r in links]
                pts_offset_review={'available':True,'span_us':(max(offsets)-min(offsets))*1e6,'first_offset_us':offsets[0]*1e6,'last_offset_us':offsets[-1]*1e6,'median_offset_us':statistics.median(offsets)*1e6,'strict_tolerance_us':10}
                check('source_pts_preserved',max(offsets)-min(offsets)<=.00001,'Source PTS not preserved modulo constant epoch offset (10us tolerance)')
        else:
            check('source_input_mapping',False,'No complete source evidence for mapping')
        if outputs:
            completion=cadence(outputs,'done_s',a.fps)
            outseq=[integer(r,'input_n') for r in outputs]
            check('output_completion_mapping',outseq==expected or (a.allow_final_eos_loss and outseq==expected[:-1]),'Retrieval IDs must match all inputs or explicitly allowed final EOS loss')
            check('output_cadence',completion.get('strictly_increasing',False) and abs(completion.get('fps',0)/a.fps-1)<=a.rate_tolerance,'Retrieved-output cadence outside tolerance')
            expected_pts={integer(r,'seq'):integer(r,'pts') for r in ts}
            check('output_pts_mapping',all(integer(r,'pts_us')==expected_pts.get(integer(r,'input_n')) for r in outputs),'Retrieved output PTS differs from matched input')
            completion['stage']=sorted(set(r.get('stage','unspecified') for r in outputs))
            completion['retrieved_but_not_decoded_sequences']=sorted(set(outseq)-set(visible))
            if links and captures and checks.get('source_input_mapping'):
                capbyinput={integer(r,'input_n'):byseq[integer(r,'source_seq')] for r in links}
                latency=[(float(r['done_s'])-float(capbyinput[integer(r,'input_n')]['host_s']))*1000 for r in outputs]
                completion['dequeue_to_retrieval_ms']={'min':min(latency),'max':max(latency),'median':statistics.median(latency)}
                n=len(latency);third=max(1,n//3)
                completion['latency_third_medians_ms']=[statistics.median(latency[:third]),statistics.median(latency[third:2*third]),statistics.median(latency[2*third:])] if n>=3 else []
        else:check('output_completion_mapping',False,'No CAM_OUTPUT post-drain completion records')
    except (KeyError,ValueError,ZeroDivisionError) as e:check('source_schema_valid',False,'Malformed source/output schema: '+str(e))
    steady={'available':False,'fixed_warmup_seconds':a.warmup_seconds}
    if captures and checks.get('capture_fields') and checks.get('source_input_mapping'):
        cutoff=float(captures[0]['driver_s'])+a.warmup_seconds
        sc=[r for r in captures if float(r['driver_s'])>=cutoff]
        if len(sc)>=2:
            sq=[integer(r,'source_seq') for r in sc];validseq=set(sq)
            sl=[r for r in links if integer(r,'source_seq') in validseq]
            ln=[integer(r,'input_n') for r in sl];validinput=set(ln)
            so=[r for r in outputs if integer(r,'input_n') in validinput]
            dc=cadence(sc,'driver_s',a.fps);hc=cadence(sc,'host_s',a.fps)
            gaps=[{'previous':x,'current':y} for x,y in zip(sq,sq[1:]) if y!=x+1]
            steady={'available':True,'fixed_warmup_seconds':a.warmup_seconds,'first_source_sequence':sq[0], 'capture_count':len(sc),'source_sequence_gaps':gaps,'driver_capture':dc,'host_capture':hc,'output_retrieval':cadence(so,'done_s',a.fps),'linked_inputs':len(sl),'retrieved_outputs':len(so)}
            steady['source_cadence_and_continuity_pass']=not gaps and dc['strictly_increasing'] and hc['strictly_increasing'] and abs(dc['fps']/a.fps-1)<=a.rate_tolerance and abs(hc['fps']/a.fps-1)<=a.rate_tolerance
            steady['minimum_duration_pass']=dc['duration_s']+1/a.fps>=a.min_seconds
            if so:
                cap_by_n={integer(r,'input_n'):byseq[integer(r,'source_seq')] for r in sl}
                latency=[(float(r['done_s'])-float(cap_by_n[integer(r,'input_n')]['host_s']))*1000 for r in so]
                third=max(1,len(latency)//3)
                steady['dequeue_to_retrieval_ms']={'min':min(latency),'max':max(latency),'median':statistics.median(latency)}
                steady['latency_third_medians_ms']=[statistics.median(latency[:third]),statistics.median(latency[third:2*third]),statistics.median(latency[2*third:])] if len(latency)>=3 else []
                steady['latency_last_minus_first_third_ms']=steady['latency_third_medians_ms'][-1]-steady['latency_third_medians_ms'][0] if len(latency)>=3 else None
                for seconds in (10,30):
                    block=round(a.fps*seconds);tsout=[float(r['done_s']) for r in so]
                    steady[str(seconds)+'s_retrieval_fps']=[block/(tsout[i+block]-tsout[i]) for i in range(0,len(tsout)-block,block)]
            steady['qualification_note']='Steady cadence/duration are separate from fullstream startup loss, decoded EOS accounting, image quality and resource review; not a full system pass.'
    result={'log':a.log,'glyph_validation':a.glyph_validation,'checks':checks,'automated_evidence_pass':all(checks.values()),'missing_evidence_or_failures':issues,'expected_frames':len(expected),'decoded_frames':len(visible),'missing_sequences':missing,'allowed_final_missing_sequence':allowed,'source_pts_offset_review':pts_offset_review,'steady':steady,'driver_capture':driver,'host_capture':host,'output_completion':completion,'manual_review_still_required':['Real changing imagery outside OSD; hardware timestamp semantics and sensor register timing','DMA-BUF export/import correlation and raw image copy audit','Output lag windows, sustained CPU/memory/swap/reclaim/thermal metrics','Restart and retained 30fps path; no complete system acceptance from this boolean alone']}
    Path(a.output).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));return 0 if result['automated_evidence_pass'] else 2
if __name__=='__main__':raise SystemExit(main())
