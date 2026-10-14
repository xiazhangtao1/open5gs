const mongoose = require('mongoose');
const Schema = mongoose.Schema;
require('mongoose-long')(mongoose);
const { MAX_IPV4_FRAMED_ROUTES, IPV4_CIDR_PATTERN } = require('../../src/helpers/framed-routes');
const ipv4Cidr = new RegExp(IPV4_CIDR_PATTERN);

const Subscriber = new Schema({

  schema_version: {
    $type: Number,
    default: 1  // Current Schema Version
  },

  imsi: { $type: String, unique: true, required: true },

  msisdn: [ String ],
  imeisv: [ String ],
  mme_host: [ String ],
  mme_realm: [ String ],
  purge_flag: [ Boolean ],

  security: {
    k: String,
    op: String,
    opc: String,
    amf: String,
    rand: String,
    sqn: Schema.Types.Long
  },

  ambr: {
    downlink: { value: Number, unit: Number },
    uplink: { value: Number, unit: Number }
  },

  slice: [{
    sst: { $type: Number, required: true },
    sd: String,
    default_indicator: Boolean,
    session: [{
      name: { $type: String, required: true }, // DNN or APN
      type: Number,
      qos: {
        index: Number, // 5QI or QCI
        arp: {
          priority_level: Number,
          pre_emption_capability: Number,
          pre_emption_vulnerability: Number,
        }
      },
      ambr: {
        downlink: { value: Number, unit: Number },
        uplink: { value: Number, unit: Number }
      },
      ue: {
        ipv4: String,
        ipv6: String
      },
      smf: {
        ipv4: String,
        ipv6: String
      },
      ipv4_framed_routes: {
        $type: [String],
        default: undefined,
        set: routes => {
          if (routes !== undefined && !Array.isArray(routes)) {
            throw new mongoose.Error.CastError('Array', routes, 'ipv4_framed_routes');
          }
          return routes;
        },
        validate: {
          validator: routes => routes === undefined || (
            Array.isArray(routes) && routes.length <= MAX_IPV4_FRAMED_ROUTES &&
            routes.every(route => typeof route === 'string' && ipv4Cidr.test(route))
          ),
          message: 'Use up to 8 IPv4 CIDR routes per session, e.g. 192.168.0.0/16'
        }
      },
      pcc_rule: [{
        flow: [{
          direction: Number,
          description: String
        }],
        qos: {
          index: Number, // 5QI or QCI
          arp: {
            priority_level: Number,
            pre_emption_capability: Number,
            pre_emption_vulnerability: Number,
          },
          mbr: {
            downlink: { value: Number, unit: Number },
            uplink: { value: Number, unit: Number }
          },
          gbr: {
            downlink: { value: Number, unit: Number },
            uplink: { value: Number, unit: Number }
          },
        },
      }],

      lbo_roaming_allowed: Boolean

    }]
  }],

  access_restriction_data: {
    $type: Number,
    default: 32 // Handover to Non-3GPP Access Not Allowed
  },
  subscriber_status: {
    $type: Number,
    default: 0  // Service Granted
  },
  operator_determined_barring: {
    $type: Number,
    default: 0 // No barring
  },
  network_access_mode: {
    $type: Number,
    default: 0 // Packet and Circuit
  },
  subscribed_rau_tau_timer: {
    $type: Number,
    default: 12 // minites
  }

}, { typeKey: '$type' });

module.exports = mongoose.model('Subscriber', Subscriber);
