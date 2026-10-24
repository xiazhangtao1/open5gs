// Resolve roles from MongoDB on each management request, never trust cached JWT roles.
module.exports = function requireAdmin(Account = require('./models/account')) {
  return async (req, res, next) => {
    if (!req.user || !req.user._id) return res.status(401).json({error: '请先登录'});
    try {
      const account = await Account.findById(req.user._id).select('username roles').lean();
      if (!account || !Array.isArray(account.roles) || !account.roles.includes('admin'))
        return res.status(403).json({error: '仅管理员可以执行此操作'});
      req.controlActor = account.username;
      next();
    } catch (e) { res.status(503).json({error: '暂时无法校验账号权限'}); }
  };
};
