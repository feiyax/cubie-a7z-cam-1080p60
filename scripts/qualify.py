#!/usr/bin/env python3
"""Bounded on-target Auto-exposure audit. Run under the shared lock and sudo.

Requires the optional vendor-SDK audit build. Does not change ISP controls or
receiver tuning. Saves private evidence under ignored runs/; do not publish it.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import signal
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('label', help='New run directory name, letters/digits/hyphens only')
p.add_argument('--fps', type=int, choices=(30, 60), default=60)
p.add_argument('--frames', type=int, default=11100)
p.add_argument('--timeout', type=float, help='Default: nominal duration + 30 seconds')
p.add_argument('--output-root', type=pathlib.Path, default=ROOT/'runs')
p.add_argument('--build-dir', type=pathlib.Path, default=os.environ.get('OUTPUT_DIR', ROOT/'out'))
p.add_argument('--context-dir', type=pathlib.Path, default='/mnt', help='Vendor ISP saved-context directory')
p.add_argument('--inject-write-delay-ms', type=int, default=0, help='Test only: one scoped compressed-write stall')
a = p.parse_args()
if not a.label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-' for c in a.label):
    p.error('label must contain only letters, digits and hyphens')
if a.frames < 1 or a.frames > 10000000 or a.inject_write_delay_ms not in (0, 650):
    p.error('invalid frame count or delay (test delay must be 0 or 650 ms)')
if os.geteuid() != 0:
    p.error('run under sudo: Cedar/DMA devices require root on the tested image')
a.build_dir = a.build_dir.resolve()
a.output_root.mkdir(parents=True, exist_ok=True)
filesystem = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(a.output_root)], text=True).strip()
if filesystem in ('tmpfs', 'ramfs'):
    p.error('evidence must be written to disk, not RAM-backed storage')
out = a.output_root.resolve()/a.label
out.mkdir(exist_ok=False)
def call(cmd):
    r = subprocess.run(cmd, text=True, capture_output=True)
    return dict(cmd=cmd, rc=r.returncode, out=r.stdout, err=r.stderr)
def read(path):
    try: return pathlib.Path(path).read_text()
    except OSError: return None
def health():
    return dict(mono=time.monotonic(), stat=read('/proc/stat'), meminfo=read('/proc/meminfo'),
                vmstat=read('/proc/vmstat'), interrupts=read('/proc/interrupts'),
                mpp_vi=read('/sys/kernel/debug/mpp/vi'), mpp_mipi=read('/sys/kernel/debug/mpp/mipi'),
                thermal={str(x):x.read_text().strip() for x in pathlib.Path('/sys/class/thermal').glob('thermal_zone*/temp')})
nodes={x.read_text().strip():'/dev/'+x.parent.name for x in pathlib.Path('/sys/class/video4linux').glob('v4l-subdev*/name')}
if 'sunxi_isp.0' not in nodes or 'imx214' not in nodes:
    p.error('expected ISP and IMX214 subdevices are unavailable')
controls=[]
for name in ('sunxi_isp.0', 'sunxi_isp.1'):
    if name not in nodes: continue
    result=call(['v4l2-ctl','-d',nodes[name],'--get-ctrl=auto_exposure,exposure_time_absolute,gain_automatic,gain,white_balance_automatic'])
    controls.append(result)
    if result['rc'] or 'auto_exposure: 0' not in result['out'] or 'gain_automatic: 1' not in result['out']:
        p.error('qualification requires preexisting Auto exposure and automatic gain; controls are not changed')
libs=[a.build_dir/'benchmark_audit.so', a.build_dir/'sink_write_trace.so']
if a.inject_write_delay_ms: libs.append(a.build_dir/'sink_delay_test.so')
for lib in libs:
    if not lib.is_file(): p.error('missing audit library: '+str(lib))
video=out/'video.h264'
cmd=['bash',str(ROOT/'scripts/with-camera60-fixes.sh'),'gst-launch-1.0','--no-fault','-e',
     'v4l2src','device=/dev/video0','en-awisp=1','en-largemode=0','io-mode=dmabuf','num-buffers='+str(a.frames),'!',
     f'video/x-raw,format=NV12,width=1920,height=1080,framerate={a.fps}/1','!',
     'queue','max-size-buffers=4','max-size-bytes=0','max-size-time=0','!',
     'omxh264videoenc','target-bitrate=12000000','!',
     'queue','max-size-buffers=0','max-size-bytes=4194304','max-size-time=0','!',
     'filesink','location='+str(video)]
env=os.environ.copy()
env.pop('LD_PRELOAD', None)
env.update(OUTPUT_DIR=str(a.build_dir), A733_EXTRA_PRELOAD=':'.join(map(str,libs)),
           A733_TIMESTAMPS='1', A733_TIMESTAMP_MODE='dynamic', A733_SINK_TRACE_PATH=str(video))
if a.inject_write_delay_ms:
    env['A733_SINK_TEST_DELAY_MS']=str(a.inject_write_delay_ms)
backups={}
for f in a.context_dir.glob('isp*_ctx_saved.bin'):
    backup=out/('preexisting-'+f.name);shutil.copy2(f,backup);backups[f.name]=backup
samples=[health()];timed_out=False;child=None;start=time.monotonic()
exposure_during_stream=[];exposure_times={3,30,60,120,180}
(out/'dmesg-before.txt').write_text(call(['dmesg'])['out'])
try:
    with (out/'capture.log').open('w') as log:
        child=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,cwd=out,start_new_session=True)
        deadline=start+(a.timeout if a.timeout is not None else a.frames/a.fps+30)
        next_sample=start+1
        while child.poll() is None:
            now=time.monotonic()
            if now>=deadline:
                timed_out=True;break
            for elapsed in sorted(exposure_times.copy()):
                if now-start>=elapsed:
                    exposure_during_stream.append(dict(mono=now, result=call(['v4l2-ctl','-d',nodes['imx214'],'--get-ctrl=exposure'])))
                    exposure_times.remove(elapsed)
            if now>=next_sample:
                sample=health();sample['process_stat']=read(f'/proc/{child.pid}/stat');samples.append(sample);next_sample=now+1
            time.sleep(.1)
finally:
    if child is not None and child.poll() is None:
        os.killpg(child.pid,signal.SIGINT)
        try:child.wait(timeout=5)
        except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait()
    for f in a.context_dir.glob('isp*_ctx_saved.bin'):
        shutil.copy2(f,out/f.name)
        if f.name not in backups:f.unlink()
    for name,backup in backups.items():shutil.copy2(backup,a.context_dir/name)
    samples.append(health())
    (out/'dmesg-after.txt').write_text(call(['dmesg'])['out'])
    result=dict(args={k:str(v) if isinstance(v,pathlib.Path) else v for k,v in vars(a).items()},
                cmd=cmd,returncode=child.returncode if child else None,timed_out=timed_out,
                wall=time.monotonic()-start,before_controls=controls,samples=samples,
                exposure_during_stream=exposure_during_stream,
                exposure_after_stop=call(['v4l2-ctl','-d',nodes['imx214'],'--get-ctrl=exposure']),
                hash={f.name:hashlib.file_digest(f.open('rb'),'sha256').hexdigest() for f in out.iterdir() if f.is_file()})
    (out/'run.json').write_text(json.dumps(result,indent=2))
print(out)
raise SystemExit(1 if timed_out or child.returncode else 0)
