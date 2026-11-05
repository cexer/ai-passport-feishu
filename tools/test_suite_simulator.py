"""Run repeatable acceptance groups with independent synthetic peer ports."""
import argparse
import datetime
import hashlib
import json
import subprocess
import sys
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--simulator',required=True)
p.add_argument('--build',required=True,type=Path)
p.add_argument('--suite',required=True,choices=['core','storage','media'])
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
groups={
    'core':[('test_simulator.py',18036),('test_outbox_simulator.py',18036),('test_priority_simulator.py',18036)],
    'storage':[('test_cache_simulator.py',18037),('test_full_text_simulator.py',18037),
               ('test_mentions_simulator.py',18038),('test_page_simulator.py',18042),
               ('test_page_auth_simulator.py',18040),('test_page_retry_simulator.py',18041)],
    'media':[('test_media_simulator.py',18035),('test_draft_manager_simulator.py',18039)],
}
digest=hashlib.sha256((a.build/'FoloToy-AI-Passport-full.bin').read_bytes()).hexdigest()
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out=root/'build/checkpoint-regression'/digest/(stamp+'-'+a.suite)
out.mkdir(parents=True)
results=[]
try:
    for script,port in groups[a.suite]:
        log=out/(script.removesuffix('.py')+'.log')
        with log.open('w',encoding='utf-8') as stream:
            completed=subprocess.run([sys.executable,str(root/'tools'/script),'--simulator',a.simulator,
                '--build',str(a.build.resolve()),'--peer-port',str(port)],stdout=stream,stderr=subprocess.STDOUT,timeout=1800)
        summaries=[]
        for line in log.read_text(encoding='utf-8').splitlines():
            if line.startswith('{"status":'):
                summaries.append(json.loads(line))
        if completed.returncode or not summaries:
            raise RuntimeError(script+' failed; see '+str(log))
        result=summaries[-1]
        if result['status']!='PASS' or result['image_sha256']!=digest:
            raise RuntimeError(script+' did not accept the exact image')
        results.append(dict(script=script,checks=len(result['checks']),report=result.get('report_directory')))
        print(script+': PASS ('+str(len(result['checks']))+')',flush=True)
    summary=dict(status='PASS',image_sha256=digest,suite=a.suite,checks=sum(r['checks'] for r in results),results=results)
except Exception as error:
    summary=dict(status='FAIL',image_sha256=digest,suite=a.suite,results=results,error=str(error))
    raise
finally:
    (out/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps(dict(summary,report_directory=str(out))),flush=True)
