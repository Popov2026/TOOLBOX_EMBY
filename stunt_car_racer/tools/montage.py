#!/usr/bin/env python3
"""Script driver for stemu. Script lines: 
  wait N | key SC | joy MASK HOLD | fire | shot NAME | every N | end"""
import sys,subprocess,os
S=os.path.dirname(os.path.abspath(__file__))+'/..'
args=[]; f=0; every=None
script=open(sys.argv[1]).read().split('\n')
extra=sys.argv[2:]
for l in script:
    p=l.split('#')[0].split()
    if not p: continue
    c=p[0]
    if c=='wait': f+=int(p[1])
    elif c=='key':
        for sc in p[1:]:
            args+=['--key','%d:%s:down'%(f,sc),'--key','%d:%s:up'%(f+10,sc)]; f+=25
    elif c=='joy':
        hold=int(p[2]) if len(p)>2 else 6
        args+=['--joy','%d:%s'%(f,p[1]),'--joy','%d:0'%(f+hold)]; f+=hold+4
    elif c=='joyon': args+=['--joy','%d:%s'%(f,p[1])]
    elif c=='shot': args+=['--shot','%d:%s'%(f,p[1])]
    elif c=='every': args+=['--shots-every',p[1]]
    elif c=='end': break
cmd=[S+'/tools/stemu','--disk',S+'/disk/scr.st','--dir',S+'/files','--frames',str(f+1)]+args+extra+[S+'/files/GAME.PUT']
r=subprocess.run(cmd,capture_output=True,text=True)
sys.stderr.write('\n'.join(l for l in r.stderr.split('\n') if 'Floprd' not in l)[-3000:])
print('frames',f)
