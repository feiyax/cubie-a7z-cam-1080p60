#!/usr/bin/env python3
"""Validate explicit agent provenance on all new commits; never infer a model version."""
import argparse, pathlib, re, subprocess, sys
FOUNDATION='ca58666dcf7403f12fa985abc9fcec9596d6b186'
FIELDS=('Agent-Name','Model-Name','Model-Variant','Model-ID','Reasoning-Effort','Model-Version')
def validate(message, author=None):
    trailers=subprocess.check_output(['git','interpret-trailers','--parse'],input=message,text=True)
    values={}; errors=[]
    for line in trailers.splitlines():
        key,_,value=line.partition(':')
        if key in FIELDS:
            if key in values:errors.append('duplicate '+key)
            values[key]=value.strip()
    for key in FIELDS:
        if not values.get(key):errors.append('missing '+key)
    if values.get('Reasoning-Effort') not in ('low','medium','high','xhigh','max','ultra','not-applicable'):
        errors.append('invalid Reasoning-Effort')
    if author and values.get('Agent-Name') and author!=values['Agent-Name']:
        errors.append('author must equal Agent-Name; do not inherit a human identity')
    return errors
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--message-file',type=pathlib.Path);p.add_argument('--range',default='HEAD')
    a=p.parse_args();failed=False
    if a.message_file:
        errors=validate(a.message_file.read_text())
        if errors:print('; '.join(errors),file=sys.stderr);return 1
        return 0
    commits=subprocess.check_output(['git','rev-list',a.range],text=True).splitlines()
    for commit in commits:
        if commit==FOUNDATION:continue
        message=subprocess.check_output(['git','show','-s','--format=%B',commit],text=True)
        author=subprocess.check_output(['git','show','-s','--format=%an',commit],text=True).strip()
        errors=validate(message,author)
        committer=subprocess.check_output(['git','show','-s','--format=%cn',commit],text=True).strip()
        if committer!=author:errors.append('committer must match explicit agent author')
        if errors:print(commit+': '+'; '.join(errors),file=sys.stderr);failed=True
    return int(failed)
if __name__=='__main__':sys.exit(main())
