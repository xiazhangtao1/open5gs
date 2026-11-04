const assert = require('assert');
const {signalIdentity, signalPeer, signalAssociation} = require('../lib/signals');
assert.strictEqual(signalIdentity({scope:'gnb'}),'GNB');
assert.strictEqual(signalIdentity({scope:'gnb',gnb_id:3584}),'GNB 3584');
assert.strictEqual(signalIdentity({scope:'gnb',gnb_id:0}),'GNB 0');
assert.strictEqual(signalIdentity({scope:'gnb',gnb_id:0xffffffff}),'GNB 4294967295');
for(const gnb_id of [null,undefined,'3584','',-1,1.5,NaN,Infinity,0x100000000])
  assert.strictEqual(signalIdentity({scope:'gnb',gnb_id}),'GNB');
for(const name of ['NGSetupRequest','NGSetupResponse','NGSetupFailure','NGReset','NGResetAcknowledge',
  'RANConfigurationUpdate','RANConfigurationUpdateAcknowledge','RANConfigurationUpdateFailure',
  'AMFConfigurationUpdate','AMFConfigurationUpdateAcknowledge','AMFConfigurationUpdateFailure',
  'UplinkRANConfigurationTransfer','DownlinkRANConfigurationTransfer']) {
  assert.strictEqual(signalIdentity({protocol:'NGAP',name}),'GNB');
  assert.strictEqual(signalIdentity({protocol:'NGAP',name,gnb_id:3584}),'GNB 3584');
}
// Never derive identities from another connection or an unadmitted request's report.
assert.strictEqual(signalIdentity({protocol:'NGAP',name:'NGSetupRequest',gnb_id:null,
  info:'peer=[10.20.0.1]:53783 reported-gNB=3584/28 PLMN=46011'}),'GNB');
assert.strictEqual(signalIdentity({protocol:'NGAP',name:'Paging',scope:'ue'}),'身份解析中');
assert.strictEqual(signalIdentity({protocol:'NGAP',name:'ErrorIndication'}),'身份解析中');
assert.strictEqual(signalIdentity({protocol:'SBI',name:'NGSetupRequest'}),'身份解析中');
assert.strictEqual(signalIdentity({scope:'unknown'}),'未关联终端');
assert.strictEqual(signalIdentity({scope:'ue'}),'身份解析中');
assert.strictEqual(signalIdentity({}),'身份解析中');
assert.strictEqual(signalIdentity({scope:'gnb',supi:'imsi-001010000000001'}),'imsi-001010000000001');
assert.strictEqual(signalPeer({peer:'[2001:db8::1]:38412'}),'[2001:db8::1]:38412');
for(const peer of ['',null,undefined,' ','-','unknown',42,'x'.repeat(129)])assert.strictEqual(signalPeer({peer}),null);
const a={boot_id:'boot-a',association_id:'18446744073709551615'};
assert.strictEqual(signalAssociation(a),'boot-a/18446744073709551615');
assert.notStrictEqual(signalAssociation(a),signalAssociation({...a,boot_id:'boot-b'}));
assert.notStrictEqual(signalAssociation(a),signalAssociation({...a,association_id:'1'}));
for(const association_id of ['',null,1,'0','-1','1e6','x'.repeat(21)])assert.strictEqual(signalAssociation({...a,association_id}),null);
assert.strictEqual(signalAssociation({association_id:'1'}),null);
console.log('NGAP identity, base-station scope, peer and connection lifetime: OK');
