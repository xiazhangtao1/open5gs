const express = require('express');
const router = express.Router();

const restify = require('express-restify-mongoose')

const Subscriber = require('../models/subscriber');
restify.serve(router, Subscriber, {
  prefix: '',
  version: '',
  idProperty: 'imsi',
  runValidators: true
});

const Profile = require('../models/profile');
restify.serve(router, Profile, {
  prefix: '',
  version: ''
});

const Account = require('../models/account');
const requireAdmin = require('../core-admin')();
router.use('/Account', (req, res, next) => {
  if (['GET', 'HEAD', 'OPTIONS'].includes(req.method)) return next();
  return requireAdmin(req, res, next);
});
restify.serve(router, Account, {
  prefix: '',
  version: '',
  idProperty: 'username'
});

module.exports = router;
