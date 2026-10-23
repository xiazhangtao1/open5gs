import asyncio
import json
import os
from pathlib import Path
import struct
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from signals import Signals, HEADER, unpack_batch
from decoder import upper_pdu, protocol_tree
from memory import MemoryPressure
from aiohttp import ClientSession, web
from server import application

BOOT = b"00000000-0000-0000-0000-000000000001"


class Budget:
    def check(self, *args):
        pass


def frame(seq=1, pdu=b"\x7e\x00\x43"):
    return HEADER.pack(b"XTR1", 2, 1, 0, seq, 1790000000000000, len(pdu), BOOT, b"AMF\0") + pdu


def ident(seq=1):
    return f"AMF/{BOOT.decode()}/{seq}"


class SignalsTest(unittest.TestCase):
    def test_wire_boundaries_and_exact_ids(self):
        rows = unpack_batch(frame((1 << 64) - 1))
        self.assertEqual(rows[0][0], ident((1 << 64) - 1))
        for bad in (frame()[:-1], frame() + b"x", b"x", frame(pdu=b""), frame(pdu=b"x" * 32769)):
            with self.assertRaises(ValueError):
                unpack_batch(bad)

    def test_capacity_no_time_expiry_and_id_collision(self):
        cache = Signals(Budget()); cache.raw_count = 2
        cache.admit(frame())
        with patch('time.time', return_value=time.time() + 86400):
            cache.trim()
        self.assertIn(ident(), cache.raw)
        cache.admit(frame(2) + frame(3))
        self.assertNotIn(ident(), cache.raw)
        self.assertEqual(cache.raw_bytes, 6)
        cache.admit(frame(3))
        self.assertEqual(len(cache.raw), 2)
        with self.assertRaises(ValueError):
            cache.admit(frame(3, b"other"))

    def test_coalescing_and_bounded_pending(self):
        cache = Signals(Budget())
        cache.admit(b"".join(frame(i) for i in range(1, 67)))
        with patch('signals.config', return_value={"enabled": True, "trace_enabled": True}):
            for _ in range(50):
                self.assertEqual(cache.query(ident(), 'decoded')["state"], 'queued')
            self.assertEqual(cache.queue.qsize(), 1)
            for i in range(2, 65):
                cache.query(ident(i), 'decoded')
            self.assertEqual(cache.query(ident(65), 'decoded')["state"], 'busy')
            self.assertEqual(cache.queue.qsize(), 64)

    def test_global_hot_stop_prevents_new_decode_jobs(self):
        cache = Signals(Budget()); cache.admit(frame())
        with patch('signals.config', return_value={"enabled": False, "trace_enabled": True}):
            self.assertEqual(cache.query(ident(), 'decoded')['reason'], 'disabled')
            self.assertEqual(cache.queue.qsize(), 0)

    def test_encoded_cache_and_memory_reclaim(self):
        cache = Signals(Budget()); cache.decoded_count = 1
        cache.admit(frame())
        cache.remember(ident(), dict(state='ready', tree=[dict(name='a')], hex='7e 00 43'))
        self.assertNotIn('hex', cache.query(ident(), 'decoded'))
        self.assertNotIn('tree', cache.query(ident(), 'hex'))
        cache.remember(ident(2), dict(state='error', reason='malformed'))
        self.assertEqual(len(cache.decoded), 1)
        cache.trim(pressure=True)
        self.assertEqual(cache.raw_bytes + cache.decoded_bytes, 0)

    def test_exported_pdu_is_not_fake_sctp(self):
        pcap, prefix = upper_pdu(b'abc', 1)
        self.assertEqual(struct.unpack_from('<I', pcap, 20)[0], 252)
        self.assertEqual(pcap[40 + prefix:], b'abc')
        self.assertIn(b'ngap\0', pcap)

    def test_secret_redaction_covers_ancestors_and_hex(self):
        xml = b'''<pdml><packet><proto name="ngap" pos="0" size="20"><field name="ngap.value" pos="8" size="8" show="SECRET"><field name="ngap.SecurityKey" pos="10" size="4" showname="SecurityKey: SECRET" show="SECRET"/></field><field name="ngap.procedureCode" pos="8" size="1" show="14"/></proto></packet></pdml>'''
        doc = protocol_tree(xml, bytes(range(12)), 8)
        self.assertEqual(doc['state'], 'ready')
        self.assertNotIn('SECRET', json.dumps(doc))
        self.assertEqual(doc['hex'], '00 01 ** ** ** ** 06 07 08 09 0a 0b')
        self.assertTrue(doc['redacted'])

    def test_partial_and_malformed_never_return_raw_hex(self):
        xml = b'<pdml><packet><proto name="ngap"><field name="_ws.expert.message" show="Unknown extension"/></proto></packet></pdml>'
        self.assertIsNone(protocol_tree(xml, b'secret', 0)['hex'])
        xml = b'<pdml><packet><proto name="ngap"/><proto name="_ws.malformed"/></packet></pdml>'
        doc = protocol_tree(xml, b'secret', 0)
        self.assertEqual(doc['state'], 'error')
        self.assertNotIn('hex', doc)

    def test_hostile_xml_and_deep_trees(self):
        from defusedxml.common import DefusedXmlException
        with self.assertRaises(DefusedXmlException):
            protocol_tree(b'<!DOCTYPE a [<!ENTITY x "boom">]><pdml>&x;</pdml>', b'', 0)
        with self.assertRaises(ValueError):
            protocol_tree(b'<pdml>' + b'<field>' * 70 + b'</field>' * 70 + b'</pdml>', b'', 0)


class SignalHTTPTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.decode_count = 0
        self.version = 'test-decoder-1'
        decoder = web.Application()
        async def decode(request):
            self.assertEqual(request.headers.get('Authorization'), 'Bearer ' + 't' * 64)
            self.assertEqual(await request.read(), b'\x7e\x00\x43')
            self.decode_count += 1
            await asyncio.sleep(.05)
            return web.json_response(dict(state='ready', tree=[dict(name='nas-5gs.mm.message_type', value='0x43')],
                                          hex='7e 00 43', decoder_version=self.version))
        async def health(request):
            return web.json_response(dict(ready=True, decoder_version=self.version))
        decoder.router.add_post('/decode', decode)
        decoder.router.add_get('/healthz', health)
        self.decoder = web.AppRunner(decoder)
        await self.decoder.setup()
        site = web.TCPSite(self.decoder, '127.0.0.1', 0); await site.start()
        url = 'http://127.0.0.1:' + str(site._server.sockets[0].getsockname()[1])
        self.env = patch.dict(os.environ, TRACE_DECODER_URL=url, MONITOR_INTERNAL_TOKEN='t' * 64)
        self.env.start()
        self.cfg = patch('signals.config', return_value=dict(enabled=True, trace_enabled=True)); self.cfg.start()
        self.app = application('t' * 64, collecting=False)
        self.app['collector'].collection_ready = True
        self.signals = self.app['cache'].signals
        self.signals.admit(frame())
        self.runner = web.AppRunner(self.app); await self.runner.setup()
        site = web.TCPSite(self.runner, '127.0.0.1', 0); await site.start()
        self.url = 'http://127.0.0.1:' + str(site._server.sockets[0].getsockname()[1])
        self.http = ClientSession(headers={'Authorization': 'Bearer ' + 't' * 64})

    async def asyncTearDown(self):
        await self.http.close(); await self.runner.cleanup(); await self.decoder.cleanup()
        self.cfg.stop(); self.env.stop()

    async def get(self, **params):
        async with self.http.get(self.url + '/api/signal', params=dict(id=ident(), **params)) as response:
            self.assertEqual(response.status, 200)
            return await response.json()

    async def ready(self):
        for _ in range(100):
            result = await self.get()
            if result['state'] == 'ready':
                return result
            await asyncio.sleep(.02)
        self.fail('decoder never became ready')

    async def test_50_readers_share_one_decode_and_do_not_poll_nf(self):
        await asyncio.gather(*(self.get() for _ in range(50)))
        await self.ready()
        results = await asyncio.gather(*(self.get() for _ in range(50)))
        self.assertTrue(all(r['state'] == 'ready' for r in results))
        self.assertEqual(self.decode_count, 1)
        self.assertFalse(self.app['collector'].tasks)
        self.assertTrue(all(s.get('requests', 0) == 0 for s in self.app['cache'].sources.values()))
        self.assertNotIn('tree', await self.get(view='hex'))
        async with self.http.get(self.url + '/api/signal', params=dict(id=ident())) as response:
            self.assertEqual(response.headers['Cache-Control'], 'no-store')

    async def test_version_change_invalidates_already_cached_message(self):
        await self.ready()
        self.version = 'test-decoder-2'
        for _ in range(65):
            if self.signals.decoder_version == self.version:
                break
            await asyncio.sleep(.1)
        self.assertEqual(self.signals.decoder_version, self.version)
        self.assertNotIn(ident(), self.signals.decoded)
        self.assertEqual((await self.ready())['decoder_version'], self.version)
        self.assertEqual(self.decode_count, 2)

    async def test_invalid_queries_and_authorization(self):
        async with ClientSession() as guest:
            async with guest.get(self.url + '/api/signal', params=dict(id=ident())) as response:
                self.assertEqual(response.status, 401)
        for query in ({'id': '../../AMF'}, {'id': ident(), 'view': 'keys'}):
            async with self.http.get(self.url + '/api/signal', params=query) as response:
                self.assertEqual(response.status, 400)


@unittest.skipUnless(os.getenv('TRACE_FIXTURES'), 'set TRACE_FIXTURES to real codec-generated PDU directory')
class RealDecoderTests(unittest.IsolatedAsyncioTestCase):
    async def test_ngap_nas_nested_transfer_and_redaction(self):
        from decoder import run_tshark
        directory = Path(os.environ['TRACE_FIXTURES'])
        for name, kind in [('registration-complete.bin', 2), ('session-establishment-request.bin', 3),
                           ('ul-nas-transport.bin', 2), ('initial-context-key.bin', 1),
                           ('session-setup-transfer.bin', 1)]:
            doc = await asyncio.wait_for(run_tshark((directory / name).read_bytes(), kind), 4)
            self.assertEqual(doc['state'], 'ready', name)
            nodes, todo = [], list(doc['tree'])
            while todo:
                node = todo.pop(); nodes.append(node); todo.extend(node.get('children', []))
            fields = {n['name']: n for n in nodes if n['name']}
            if name == 'initial-context-key.bin':
                self.assertTrue(doc['redacted'])
                self.assertNotIn('a5', doc['hex'].split())
                self.assertEqual(fields['ngap.SecurityKey']['value'], '已遮蔽')
            if name == 'session-setup-transfer.bin':
                self.assertEqual(fields['ngap.gTP_TEID']['value'], 'de:ad:be:ef')
                self.assertEqual(fields['ngap.qosFlowIdentifier']['value'], '1')
                self.assertEqual(fields['ngap.fiveQI']['value'], '9')
                start = fields['ngap.gTP_TEID']['offset']
                self.assertEqual(doc['hex'].split()[start:start + 4], ['de', 'ad', 'be', 'ef'])
            if name == 'ul-nas-transport.bin':
                self.assertIn('nas-5gs.mm.message_type', fields)
                self.assertIn('nas-5gs.sm.message_type', fields)


if __name__ == '__main__':
    unittest.main()
