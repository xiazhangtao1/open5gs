// Keep transport identity separate from the gNB's configured logical identity.
function sctpPeer(gnb) {
  if (!gnb || typeof gnb !== 'object') return null;
  const peer = 'peer' in gnb ? gnb.peer : ((gnb.ng || {}).sctp || {}).peer;
  return typeof peer === 'string' && peer.length <= 128 && peer.trim() && !['-', 'unknown'].includes(peer) ? peer : null;
}
function currentGnb(ue) {
  return ue && ue.cm_state === 'connected' && ue.gnb && ue.gnb.status === 'associated' ? ue.gnb : null;
}
function gnbFilters(gnb) {
  const peer = sctpPeer(gnb);
  return gnb && peer && Number.isInteger(gnb.gnb_id) && Number.isInteger(gnb.gnb_id_bits) &&
    gnb.gnb_id_bits >= 22 && gnb.gnb_id_bits <= 32 && gnb.gnb_id >= 0 && gnb.gnb_id < 2 ** gnb.gnb_id_bits &&
    typeof gnb.plmn === 'string' && gnb.plmn && gnb.plmn !== '-' ?
    {gnb_id: String(gnb.gnb_id), gnb_id_bits: String(gnb.gnb_id_bits), plmn: gnb.plmn, sctp_peer: peer} : null;
}
function gnbKey(gnb) {
  const filters = gnbFilters(gnb);
  return filters ? JSON.stringify([filters.plmn, filters.gnb_id, filters.gnb_id_bits, filters.sctp_peer]) : null;
}
module.exports = {sctpPeer, currentGnb, gnbFilters, gnbKey};
