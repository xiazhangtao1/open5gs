"""Isolated 1024-history fixture; 50 readers each choose 8 objects via HTTP.
serve uses synthetic full raw/minute rings and the real JSON ingress every 2s.
read uses SOAK_URL and SOAK_TOKEN; never run the fixture in production.
"""
import argparse
from array import array
import asyncio
import json
import os
from pathlib import Path
import sys
import time
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parent)]
from aiohttp import web,ClientSession,ClientTimeout
from server import application
from trends import Curve
from test_bearer_trends import session

def fixture():
    rows=[session(i,tuple(range(1,17)) if i<=8 else ()) for i in range(1,321)]
    rows[-1]['supi']=rows[0]['supi']
    for i,row in enumerate(rows):row['ue_ip']='10.45.'+str(i//250)+'.'+str(i%250+1)
    return rows

async def producer_context(app):
    c=app['cache'];rows=fixture();now=int(time.time()//2)*2
    c.replace('upf_rates',rows,2);c.record_rates(now,'comparison-soak')
    assert len(c.curves)==1024
    assert sum(k.startswith('bearer:') for k in c.curves)==128
    assert sum(k.startswith('rule:') for k in c.curves)==256
    prototype=Curve()
    for stamp in range(now-49*3600,now+1,2):prototype.append(stamp,1000000,2000000)
    for curve in c.curves.values():
        curve.data=array('d',prototype.data);curve.breaks=bytearray(prototype.breaks);curve.minutes=array('d',prototype.minutes)
        for field in ('count','next','minute_count','minute_next','minute_version','last','previous'):setattr(curve,field,getattr(prototype,field))
        curve.pending=list(prototype.pending)
    del prototype
    app['collector'].collection_ready=True
    async def produce():
        await asyncio.sleep(.2)
        async with ClientSession(headers={'Authorization':'Bearer '+os.environ['MONITOR_INTERNAL_TOKEN']},timeout=ClientTimeout(total=4)) as http:
            while True:
                stamp=time.time()
                doc=dict(schema=1,ready=True,truncated=False,boot_id='comparison-soak',version=str(time.monotonic_ns()),timestamp_us=str(int(stamp*1e6)),monotonic_us=str(int(time.monotonic()*1e6)),sample_us=1000000,sessions=rows)
                async with http.post('http://127.0.0.1:9998/internal/upf',json=doc) as response:
                    assert response.status==200,(response.status,(await response.text())[:100]);await response.read()
                await asyncio.sleep(max(.01,2-(time.time()-stamp)))
    task=asyncio.create_task(produce());yield;task.cancel()
    try:await task
    except asyncio.CancelledError:pass

async def read(seconds,output):
    latency,errors,samples=array('d'),[],[];stats=dict(requests=0,error_count=0,max_response_bytes=0)
    base=os.environ['SOAK_URL'].rstrip('/')
    async with ClientSession(headers={'Authorization':'Bearer '+os.environ['SOAK_TOKEN']},timeout=ClientTimeout(total=8)) as http:
        async def get(path,params=None):
            start=time.monotonic()
            async with http.get(base+'/'+path,params=params) as response:
                body=await response.read();assert response.status==200,(response.status,path)
                latency.append((time.monotonic()-start)*1000);stats['requests']+=1;stats['max_response_bytes']=max(stats['max_response_bytes'],len(body));return json.loads(body)
        a={}
        for attempt in range(60):
            try:
                a=await get('health')
                if a['curves']==1024:break
            except Exception:pass
            await asyncio.sleep(1)
        assert a.get('curves')==1024
        pools={}
        for level in ('user','session','bearer','rule'):
            pools[level]=(await get('rates',dict(level=level,rule_scope='business',limit=100,purpose='selection')))['items'];assert len(pools[level])>=8
        start=time.monotonic();await asyncio.sleep(20);b=await get('health');baseline=time.monotonic()-start
        start=time.monotonic();finish=start+seconds
        async def reader(i):
            await asyncio.sleep(i*.025)
            level=('user','session','bearer','rule')[i%4];pool=pools[level];chosen=[pool[(i*11+j*7)%len(pool)] for j in range(8)]
            window=(1800,43200,86400)[(i//4)%3];state={};next_full=0
            while time.monotonic()<finish:
                begin=time.monotonic()
                try:
                    await get('rates',dict(level=level,rule_scope='business',limit=50))
                    status_only=window>1800 and begin<next_full
                    entries=[dict(key=r['trend_key'],supi=r['supi'],status_only=status_only,**({'cursor':state[r['trend_key']]['cursor']} if r['trend_key'] in state and 'cursor' in state[r['trend_key']] else {})) for r in chosen]
                    result=await get('trends-batch',dict(seconds=window,curves=json.dumps(entries,separators=(',',':'))));assert len(result['items'])==8
                    for item in result['items']:
                        assert item['history_state']=='recording',(level,item['key'],item['history_state'])
                        if 'points' in item:
                            assert len(item['points'])<=3000 and len(item.get('buckets',[]))<=1440;state[item['key']]=item
                    if not status_only:next_full=begin+60
                except Exception as exc:
                    stats['error_count']+=1
                    if len(errors)<100:errors.append(str(exc))
                await asyncio.sleep(max(.01,2-(time.monotonic()-begin)))
        async def observe():
            while time.monotonic()<finish:
                h=await get('health');samples.append(dict(at=time.monotonic()-start,memory=h['memory'],curves=h['curves'],trend_bytes=h['trend_bytes']))
                assert h['curves']==1024 and h['trend_bytes']<=128*1024*1024
                print(json.dumps(dict(elapsed=round(time.monotonic()-start),requests=stats['requests'],errors=stats['error_count'],used_bytes=h['memory']['used_bytes'])),flush=True)
                await asyncio.sleep(min(30,max(.01,finish-time.monotonic())))
        await asyncio.gather(*(reader(i) for i in range(50)),observe());c=await get('health');elapsed=time.monotonic()-start;ordered=sorted(latency)
        def cadence(x,y,s):return {k:round((v['requests']-x['sources'].get(k,{}).get('requests',0))/s,4) for k,v in y['sources'].items()}
        result=dict(**stats,readers=50,objects_each=8,seconds=elapsed,p95_ms=ordered[int(len(ordered)*.95)],p99_ms=ordered[int(len(ordered)*.99)],errors=errors,baseline_cadence=cadence(a,b,baseline),load_cadence=cadence(b,c,elapsed),after=c,memory_samples=samples)
        Path(output).write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k not in ('after','memory_samples')}),flush=True)
        assert not stats['error_count'];assert .4<=result['load_cadence']['upf_rates']<=.6

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=('serve','read'));parser.add_argument('--seconds',type=int,default=1800);parser.add_argument('--output',default='/tmp/comparison-soak.json');args=parser.parse_args()
    if args.mode=='serve':
        app=application(collecting=False);app.cleanup_ctx.append(producer_context);web.run_app(app,host='0.0.0.0',port=9998,access_log=None)
    else:asyncio.run(read(args.seconds,args.output))
