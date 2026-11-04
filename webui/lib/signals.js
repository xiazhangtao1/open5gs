// Known non-UE names also cover cached records produced before scope was added.
const GNB_MESSAGES = new Set([
  'NGSetupRequest', 'NGSetupResponse', 'NGSetupFailure', 'NGReset', 'NGResetAcknowledge',
  'RANConfigurationUpdate', 'RANConfigurationUpdateAcknowledge', 'RANConfigurationUpdateFailure',
  'AMFConfigurationUpdate', 'AMFConfigurationUpdateAcknowledge', 'AMFConfigurationUpdateFailure',
  'UplinkRANConfigurationTransfer', 'DownlinkRANConfigurationTransfer'
]);
function signalIdentity(event) {
  if (event.supi) return event.supi;
  if (event.scope === 'gnb' || (event.protocol === 'NGAP' && GNB_MESSAGES.has(event.name))) {
    return Number.isInteger(event.gnb_id) && event.gnb_id >= 0 && event.gnb_id <= 0xffffffff ?
      'GNB ' + event.gnb_id : 'GNB';
  }
  if (event.scope === 'unknown') return '未关联终端';
  return '身份解析中';
}
function signalPeer(event) {
  return typeof event.peer === 'string' && event.peer.trim() && event.peer.length <= 128 &&
    !['-', 'unknown'].includes(event.peer) ? event.peer : null;
}
function signalAssociation(event) {
  return event.boot_id && typeof event.association_id === 'string' && /^[1-9]\d{0,19}$/.test(event.association_id) ?
    event.boot_id + '/' + event.association_id : null;
}
module.exports = {signalIdentity, signalPeer, signalAssociation};
