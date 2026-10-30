const assert = require('assert');
const {sctpPeer, currentGnb, gnbFilters, gnbKey} = require('../lib/access');
const identity = {gnb_id:3584,gnb_id_bits:28,plmn:'46011'};
const direct = {...identity,peer:'[10.2.0.119]:53783',status:'associated'};
const row = peer => ({...identity,ng:{sctp:{peer}}});
const active = {cm_state:'connected',gnb:direct,location:{gnb:{...direct,peer:'[10.2.0.120]:53783'}}};
assert.strictEqual(sctpPeer(currentGnb(active)),direct.peer);
assert.deepStrictEqual(gnbFilters(currentGnb(active)),{gnb_id:'3584',gnb_id_bits:'28',plmn:'46011',sctp_peer:direct.peer});
assert.strictEqual(gnbKey(direct),gnbKey(row(direct.peer)));
for(const state of ['idle','unknown','disconnected'])assert.strictEqual(currentGnb({...active,cm_state:state}),null);
for(const status of ['not-connected',null,undefined])assert.strictEqual(currentGnb({...active,gnb:{...direct,status}}),null);
for(const peer of [null,undefined,'','-','unknown',' ','x'.repeat(129)]){
  assert.strictEqual(sctpPeer({...direct,peer}),null);assert.strictEqual(gnbFilters({...direct,peer}),null);
}
assert.strictEqual(sctpPeer('invalid'),null);assert.strictEqual(gnbKey(null),null);
for(const peer of ['[10.2.0.120]:53783','[10.2.0.119]:53784','[2001:db8::1]:53783'])assert.notStrictEqual(gnbKey(direct),gnbKey(row(peer)));
for(const [key,value] of [['gnb_id',3585],['gnb_id_bits',27],['plmn','46012']])assert.notStrictEqual(gnbKey(direct),gnbKey({...direct,[key]:value}));
assert.strictEqual(gnbFilters({...direct,gnb_id_bits:21}),null);assert.strictEqual(gnbFilters({...direct,gnb_id:2**28}),null);
assert.strictEqual(gnbFilters({...direct,plmn:''}),null);assert.strictEqual(gnbFilters({...direct,gnb_id:true}),null);
assert(gnbFilters({...direct,gnb_id:0}));
console.log('Current SCTP peer, full transport identity, fail-closed filters and duplicate gNB IDs: OK');
