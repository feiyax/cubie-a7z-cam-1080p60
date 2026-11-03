#!/usr/bin/env python3
"""Commit already staged changes using explicitly supplied agent/model metadata."""
import argparse, os, pathlib, subprocess, tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--message-file',type=pathlib.Path,required=True)
p.add_argument('--agent-name',required=True);p.add_argument('--agent-email',required=True)
p.add_argument('--model-name',required=True);p.add_argument('--model-variant',required=True)
p.add_argument('--model-id',required=True);p.add_argument('--reasoning-effort',required=True)
p.add_argument('--model-version',required=True,help='Use not-exposed when the exact snapshot is unavailable')
a=p.parse_args();fields={'Agent-Name':a.agent_name,'Model-Name':a.model_name,'Model-Variant':a.model_variant,'Model-ID':a.model_id,'Reasoning-Effort':a.reasoning_effort,'Model-Version':a.model_version}
if any(not v.strip() or '\n' in v or '\r' in v for v in [a.agent_email,*fields.values()]):p.error('Metadata must be nonempty single-line values')
message=a.message_file.read_text().rstrip()+'\n\n'+'\n'.join(k+': '+v for k,v in fields.items())+'\n'
env=os.environ.copy();env.update(GIT_AUTHOR_NAME=a.agent_name,GIT_COMMITTER_NAME=a.agent_name,GIT_AUTHOR_EMAIL=a.agent_email,GIT_COMMITTER_EMAIL=a.agent_email)
with tempfile.NamedTemporaryFile(mode='w',suffix='.txt') as f:
    f.write(message);f.flush()
    subprocess.run(['python3',str(pathlib.Path(__file__).with_name('check-commit-metadata.py')),'--message-file',f.name],check=True)
    subprocess.run(['git','commit','--file',f.name],env=env,check=True)
