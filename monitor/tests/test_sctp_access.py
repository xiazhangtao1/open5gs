"""Transport-specific access association when all configured gNB IDs collide."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aiohttp import web, ClientSession
from cache import Cache, current_gnb, sctp_peer
from memory import MemoryBudget, MIB
from server import application
from test_monitor import fixtures

A, B = '[192.0.2.10]:38412', '[192.0.2.11]:38412'


def ue(suffix, peer, state='connected', reported=None):
    row = fixtures.ue(state=state)
    row['supi'] = 'imsi-' + str(suffix)
    row['gnb']['peer'] = peer
    row['location']['gnb']['peer'] = peer if reported is None else reported
    return row


def gnb(peer):
    return dict(gnb_id=100, gnb_id_bits=28, plmn='00101', ng=dict(setup_success=True, sctp=dict(peer=peer)))


def cache(ues, gnbs):
    c = Cache(MemoryBudget(limit=512*MIB, usage=lambda:32*MIB))
    c.replace('amf_ues', ues, 10)
    c.replace('amf_gnbs', gnbs, 15)
    return c


class SCTPAccessTests(unittest.TestCase):
    def test_identical_ids_and_nci_on_different_servers(self):
        c = cache([ue(1,A),ue(2,B),ue(3,A,'idle')], [gnb(A),gnb(B)])
        self.assertEqual([[u['supi'] for u in g['associated_ues']] for g in c.gnbs], [['imsi-1'],['imsi-2']])
        self.assertEqual([g['observed_cells'][0]['ue_count'] for g in c.gnbs], [2,1])
        self.assertEqual(c.gnbs[0]['observed_cells'][0]['idle_ue_count'], 1)
        self.assertEqual(c.ues[0]['location']['nr_cgi']['nci'], c.ues[1]['location']['nr_cgi']['nci'])
        self.assertEqual(c.query('ues', {'sctp_peer':A})['items'][0]['supi'], 'imsi-1')

    def test_same_ip_different_ports_and_ipv6(self):
        for a,b in ((A,'[192.0.2.10]:38413'),('[2001:db8::1]:38412','[2001:db8::1]:38413')):
            with self.subTest(a=a,b=b):
                c = cache([ue(1,a),ue(2,b)], [gnb(a),gnb(b)])
                self.assertEqual([len(g['associated_ues']) for g in c.gnbs], [1,1])
                for peer,supi in ((a,'imsi-1'),(b,'imsi-2')):
                    self.assertEqual([r['supi'] for r in c.query('ues', {'sctp_peer':peer})['items']], [supi])
                    self.assertEqual([sctp_peer(r) for r in c.query('gnbs', {'sctp_peer':peer})['items']], [peer])

    def test_missing_peers_never_wildcard_or_fall_back_to_location(self):
        for missing in (None,'','-','unknown',' ','x'*129):
            with self.subTest(missing=missing):
                c = cache([ue(1,missing,reported=A)], [gnb(A),gnb(missing)])
                self.assertEqual([len(g['associated_ues']) for g in c.gnbs], [0,0])
                self.assertEqual(c.query('ues', {'sctp_peer':A})['total'], 0)
                self.assertEqual(c.gnbs[0]['observed_cells'][0]['ue_count'], 1)
                self.assertEqual(c.gnbs[1]['observed_cells'], [])
        for state,status in (('idle','associated'),('unknown','associated'),('connected','not-connected'),('connected',None)):
            row=ue(1,A,state);row['gnb']['status']=status
            c=cache([row],[gnb(A)])
            self.assertIsNone(current_gnb(c.ues[0]))
            self.assertEqual(c.query('ues', {'sctp_peer':A})['total'],0)
            self.assertEqual(c.gnbs[0]['associated_ues'],[])

    def test_current_and_last_location_have_separate_transport_identity(self):
        c=cache([ue(1,B,reported=A)],[gnb(A),gnb(B)])
        self.assertEqual([len(g['associated_ues']) for g in c.gnbs],[0,1])
        self.assertEqual([len(g['observed_cells']) for g in c.gnbs],[1,0])
        self.assertEqual(c.query('ues', {'sctp_peer':A})['total'],0)
        self.assertEqual(c.query('ues', {'sctp_peer':B})['total'],1)

    def test_logical_identity_still_required_and_old_filters_compatible(self):
        others=[]
        for name,value in (('gnb_id',101),('gnb_id_bits',27),('plmn','00102')):
            other=gnb(A);other[name]=value;others.append(other)
        c=cache([ue(1,A),ue(2,A,'idle')],others+[gnb(A)])
        self.assertEqual([len(g['associated_ues']) for g in c.gnbs],[0,0,0,1])
        self.assertEqual([len(g['observed_cells']) for g in c.gnbs],[0,0,0,1])
        filters=dict(gnb_id='100',gnb_id_bits='28',plmn='00101')
        self.assertEqual(c.query('ues',filters)['total'],2)
        self.assertEqual(c.query('ues',dict(filters,sctp_peer=A))['total'],1)
        self.assertEqual(c.query('gnbs',dict(filters,sctp_peer=B))['total'],0)

    def test_reconnect_pagination_and_readers_do_not_query_sources(self):
        c=cache([ue(1,A)],[gnb(A),gnb(B)])
        self.assertEqual(c.query('gnbs', {'page':'1','limit':'1'})['items'][0]['ng']['sctp']['peer'],B)
        self.assertEqual(c.query('gnbs', {'sctp_peer':A,'limit':'2'})['total'],1)
        c.replace('amf_gnbs',[gnb(B)],15)
        self.assertEqual(c.query('gnbs', {'sctp_peer':A})['total'],0)
        self.assertEqual(c.gnbs[0]['associated_ues'],[])
        c.replace('amf_ues',[ue(1,B)],10)
        self.assertEqual(c.query('ues', {'sctp_peer':A})['total'],0)
        self.assertEqual(c.query('ues', {'sctp_peer':B})['total'],1)
        requests=copy.deepcopy({k:s['requests'] for k,s in c.sources.items()})
        for _ in range(50):c.response('gnbs',{'sctp_peer':B});c.response('ues',{'sctp_peer':B})
        self.assertEqual(requests,{k:s['requests'] for k,s in c.sources.items()})


class SCTPAPITests(unittest.IsolatedAsyncioTestCase):
    async def test_peer_filter_is_read_only_and_preserves_ipv6(self):
        token='s'*64;app=application(token,collecting=False)
        c=app['cache'];c.memory=MemoryBudget(limit=512*MIB,usage=lambda:32*MIB)
        peer='[2001:db8::1]:38412';c.replace('amf_ues',[ue(1,peer),ue(2,A)],10);c.replace('amf_gnbs',[gnb(peer),gnb(A)],15)
        runner=web.AppRunner(app,access_log=None);await runner.setup()
        site=web.TCPSite(runner,'127.0.0.1',0);await site.start();base='http://127.0.0.1:'+str(site._server.sockets[0].getsockname()[1])
        try:
            async with ClientSession(headers={'Authorization':'Bearer '+token}) as http:
                for endpoint in ('ues','gnbs'):
                    async with http.get(base+'/api/'+endpoint,params={'sctp_peer':peer}) as res:
                        self.assertEqual(res.status,200);doc=await res.json();self.assertEqual(doc['total'],1)
                async with http.get(base+'/api/ues',params={'sctp_peer':'x'*129}) as res:self.assertEqual(res.status,400)
                async with http.post(base+'/api/ues',json={'sctp_peer':peer}) as res:self.assertEqual(res.status,405)
        finally:await runner.cleanup()

if __name__=='__main__':unittest.main()
