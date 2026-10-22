const express = require('express');
const router = express.Router();

const passport = require('passport');

const jwt = require('jsonwebtoken');
const secret = process.env.JWT_SECRET_KEY;

router.get('/csrf', (req, res) => {
  return res.json({csrfToken: res.locals._csrf});
})

router.get('/session', (req, res) => {
  let session = {
    clientMaxAge: 60000, // 60 seconds
    csrfToken: res.locals._csrf
  }
  if (req.user) {
    session.user = req.user
    const body = { '_id': req.user._id, 'username': req.user.username, 'roles':req.user.roles };
    const token = jwt.sign({ user: body }, secret);
    session.authToken = token
  }

  return res.json(session)
})

router.get('/monitor-token', (req, res) => {
  res.set('Cache-Control', 'no-store');
  if (!req.user) return res.status(401).json({error: '请先登录'});
  const token = jwt.sign({scope: 'monitor:read'}, secret, {
    algorithm: 'HS256', audience: 'xcn-monitor', issuer: 'xcn-webui',
    subject: String(req.user._id), expiresIn: '5m'
  });
  return res.json({token, expiresIn: 300});
});

router.post('/login', 
  passport.authenticate('local', { successRedirect: '/' }));

router.post('/logout', (req, res) => {
  req.logout(function(err) {
    if (err) { return next(err); }
    res.redirect('/');
  });
});

module.exports = router;
