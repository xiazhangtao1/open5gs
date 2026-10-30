"""Business-PDR history and bounded same-level batch protocol regression."""
import asyncio
import copy
import json
import sys
import time
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_bearer_trends import make, session, sample, trend
from cache import Cache
from memory import MIB, MemoryBudget, MemoryPressure
from server import application
from aiohttp import web, ClientSession


def batch(c, entries, now=120, seconds=1800):
    with patch('cache.time.time', return_value=now):
        body, tag = c.trends_batch(dict(curves=json.dumps(entries), seconds=str(seconds)))
    return json.loads(body)['items'], tag


class RuleTests(unittest.TestCase):
    def test_direction_zero_and_session_isolation(self):
        c = make();s = session();s['rules'][0].update(qfi=0, qer_id=0, bps='0')
        sample(c, [s, session('2')])
        self.assertEqual(trend(c, 'rule:1:1')['points'], [[120, 0, 0]])
        self.assertEqual(trend(c, 'rule:1:2')['points'], [[120, 0, 200]])
        self.assertEqual(trend(c, 'rule:2:1')['points'], [[120, 100, 0]])
        self.assertEqual(c.query('rates', {'level':'rule'})['items'][0]['history_state'], 'recording')

    def test_only_confirmed_business_and_generation_identity(self):
        c = make();s = session(qfis=(1,2))
        s['rules'][2].update(src_if=3, dst_if=0)
        del s['rules'][3]['src_if']
        sample(c, [s]);old = trend(c, 'rule:1:1')
        self.assertEqual(trend(c,'rule:1:3')['history_reason'], 'internal')
        self.assertEqual(trend(c,'rule:1:4')['history_reason'], 'unclassified')
        self.assertFalse(any(k in c.curves for k in ('rule:1:3','rule:1:4')))
        s['rules'][0]['bps']='300';sample(c,[s],122)
        self.assertEqual(trend(c,'rule:1:1',122)['epoch'],old['epoch'])
        s['rules'][0]['generation']+=1;sample(c,[s],124)
        new=trend(c,'rule:1:1',124,cursor=old['cursor'])
        self.assertTrue(new['reset']);self.assertNotEqual(new['epoch'],old['epoch'])
        s['rules'][0].update(src_if=3,dst_if=0);sample(c,[s],126)
        self.assertNotIn('rule:1:1',c.curves)
        self.assertEqual(trend(c,'rule:1:1',126)['points'],[])

    def test_release_recreate_reboot_and_rule_identity_changes(self):
        c=make();s=session();sample(c,[s]);epoch=trend(c,'rule:1:1')['epoch']
        sample(c,[],122);self.assertEqual(trend(c,'rule:1:1',122)['history_state'],'released')
        for t,boot in ((124,'a'),(126,'b'),(128,'b')):
            if t==128:s['uptime_seconds']=0
            sample(c,[s],t,boot);new=trend(c,'rule:1:1',t)['epoch']
            self.assertNotEqual(new,epoch);epoch=new
        for field,value in (('qer_id',8),('qfi',7),('direction','DL')):
            s['rules'][0][field]=value;sample(c,[s],t+2,'b');t+=2
            new=trend(c,'rule:1:1',t)['epoch'];self.assertNotEqual(epoch,new);epoch=new
        s['rules'][0]['generation']='1';sample(c,[s],t+2,'b')
        self.assertEqual(trend(c,'rule:1:1',t+2)['history_reason'],'identity')

    def test_rule_caps_no_churn_and_higher_priority(self):
        c=make();sessions=[session(i) for i in range(1,130)];sample(c,sessions)
        admitted={k:v.epoch for k,v in c.curves.items() if k.startswith('rule:')}
        self.assertEqual(len(admitted),256)
        self.assertEqual(trend(c,'rule:129:1')['history_reason'],'capacity')
        sample(c,list(reversed(sessions)),122)
        self.assertEqual(admitted,{k:v.epoch for k,v in c.curves.items() if k.startswith('rule:')})
        self.assertLessEqual(c.rule_bytes(),32*MIB)
        c=make();c.max_curves=8;sample(c,[session(qfis=(1,2))])
        sample(c,[session(qfis=(1,2)),session('2')],122)
        self.assertIn('session:2',c.curves);self.assertIn('user:imsi-2',c.curves)
        self.assertIn('bearer:2:1',c.curves);self.assertFalse(any(k.startswith('rule:') for k in c.curves))
        self.assertEqual(len(c.curves),8)
        c=make();c.max_rule_bytes=240*1024;sample(c,[session(qfis=(1,2))])
        self.assertEqual(sum(k.startswith('rule:') for k in c.curves),2)
        self.assertLessEqual(c.rule_bytes(),c.max_rule_bytes)

    def test_batch_status_cursor_and_owner_reuse(self):
        c=make();sample(c,[session()]);items,tag=batch(c,[dict(key='rule:1:1',supi='imsi-1'),dict(key='rule:1:2')])
        self.assertEqual(len(items),2);self.assertEqual(items[0]['object']['pdr_id'],1)
        self.assertEqual(items[0]['points'],[[120,100,0]])
        same,newtag=batch(c,[dict(key='rule:1:1',supi='imsi-1'),dict(key='rule:1:2')])
        self.assertEqual(tag,newtag)
        status,_=batch(c,[dict(key='rule:1:1',status_only=True)])
        self.assertNotIn('points',status[0]);self.assertEqual(status[0]['trend_epoch'],items[0]['epoch'])
        sample(c,[session()],122)
        delta,_=batch(c,[dict(key='rule:1:1',cursor=items[0]['cursor'])],122)
        self.assertFalse(delta[0]['reset']);self.assertEqual(delta[0]['points'][-1],[122,100,0])
        s=session();s['supi']='imsi-other';sample(c,[s],124)
        replaced,_=batch(c,[dict(key='rule:1:1',supi='imsi-1')],124)
        self.assertEqual(replaced[0]['history_reason'],'instance');self.assertEqual(replaced[0]['points'],[])
        missing,_=batch(c,[dict(key='rule:99:1'),dict(key='rule:1:1')],124)
        self.assertEqual(missing[0]['history_reason'],'absent');self.assertEqual(missing[1]['history_state'],'recording')

    def test_batch_validations_and_frozen_time(self):
        c=make();sample(c,[session()])
        for entries in ([],[{'key':'global'}],[{'key':'rule:1:1'}]*2,[{'key':'rule:1:1'},{'key':'session:1'}],
                        [{'key':'rule:1:1','status_only':'yes'}],[{'key':'rule:1:1','extra':1}],
                        [{'key':'rule:1:0'}],[{'key':'rule:1:65536'}],[{'key':'rule:1:1','cursor':'bad/NaN'}],
                        [{'key':'user:'+str(i)} for i in range(9)]):
            with self.assertRaises((ValueError,TypeError)):batch(c,entries)
        with patch('cache.time.time',side_effect=[179.99]+[180.1]*50):
            items=json.loads(c.trends_batch(dict(curves=json.dumps([dict(key='rule:1:1'),dict(key='rule:1:2')]),seconds='86400'))[0])['items']
        self.assertEqual(items[0]['window_end'],items[1]['window_end'])
        self.assertIsNone(c._batch_now)
        source_requests=sum(s['requests'] for s in c.sources.values())
        for _ in range(50):batch(c,[dict(key='rule:1:1')])
        self.assertEqual(source_requests,sum(s['requests'] for s in c.sources.values()))

    def test_oldest_inactive_eviction_and_status_for_all_levels(self):
        c=make();c.max_rule_curves=2;sample(c,[session()])
        c.curves['rule:1:1'].last=121
        for level,key in [('user','user:imsi-1'),('session','session:1'),('bearer','bearer:1:1'),('rule','rule:1:1')]:
            full,_=batch(c,[dict(key=key)])
            status,_=batch(c,[dict(key=key,status_only=True)])
            self.assertEqual(full[0]['history_state'],'recording')
            self.assertEqual(full[0]['trend_epoch'],status[0]['trend_epoch'])
        s=session();s['rules']=[dict(s['rules'][0],pdr_id=3)]
        sample(c,[s],126)
        self.assertIn('rule:1:1',c.curves);self.assertNotIn('rule:1:2',c.curves)
        self.assertIn('rule:1:3',c.curves)

    def test_selection_projection_is_bounded(self):
        c=make();sample(c,[session()])
        for level in ('user','session','bearer','rule'):
            row=c.query('rates',dict(level=level,purpose='selection'))['items'][0]
            self.assertNotIn('rules',row);self.assertNotIn('qos_flows',row);self.assertIn('trend_key',row)
        c.memory.usage=lambda:400*MIB
        with self.assertRaises(MemoryPressure):batch(c,[dict(key='rule:1:1')])
        self.assertEqual(sum(k.startswith('rule:') for k in c.curves),0)


class BatchAPITests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app=application('t'*64,collecting=False);self.app['collector'].collection_ready=True
        c=self.app['cache'];c.memory=MemoryBudget(limit=512*MIB,usage=lambda:32*MIB)
        sample(c,[session()],time.time())
        self.runner=web.AppRunner(self.app,access_log=None);await self.runner.setup()
        site=web.TCPSite(self.runner,'127.0.0.1',0);await site.start()
        self.base='http://127.0.0.1:'+str(site._server.sockets[0].getsockname()[1])
        self.http=ClientSession(headers={'Authorization':'Bearer '+'t'*64})
    async def asyncTearDown(self):
        await self.http.close();await self.runner.cleanup()
    async def test_http_limits_etag_and_no_collection(self):
        params=dict(curves=json.dumps([dict(key='rule:1:1'),dict(key='rule:1:2')]))
        async with self.http.get(self.base+'/api/trends-batch',params=params) as response:
            self.assertEqual(response.status,200);tag=response.headers['ETag'];self.assertEqual(len((await response.json())['items']),2)
        async with self.http.get(self.base+'/api/trends-batch',params=params,headers={'If-None-Match':tag}) as response:self.assertEqual(response.status,304)
        for curves in ('{','[]',json.dumps([dict(key='rule:1:1')]*9)):
            async with self.http.get(self.base+'/api/trends-batch',params=dict(curves=curves)) as response:self.assertEqual(response.status,400)
        async with self.http.get(self.base+'/api/trends-batch',params=dict(curves='x'*4100)) as response:self.assertEqual(response.status,400)
        async with self.http.post(self.base+'/api/trends-batch',json=params) as response:self.assertEqual(response.status,405)
        async with ClientSession() as plain:
            async with plain.get(self.base+'/api/trends-batch',params=params) as response:self.assertEqual(response.status,401)

if __name__=='__main__':unittest.main()
