# WebUI 配置 IPv4 后路由

登录 WebUI，编辑目标订阅用户，在对应切片的 **Session Configurations** 中找到
目标 DNN/APN，在 **IPv4 Framed Routes** 下添加路由，例如 `192.168.0.0/16`，然后保存。
该列表支持增加、修改、删除；删除全部条目并保存即可清空后路由。用户详情也显示路由。

路由表示 UE 后方的网络，不是 UE 自身地址，也不需要填写下一跳。
每个 Session 最多 8 条，地址必须为 IPv4 CIDR，前缀范围为 `0–32`。
已有数据库配置会自动回显，无需迁移。已有 UE 会话应重新建立后再验证路由生效；
此功能不主动断开会话或向在线会话即时下发路由。

数据仍保存为 `slice[].session[].ipv4_framed_routes` 字符串数组，使用原有订阅用户
API。保存接口与表单均校验格式及条目上限。本次不扩展 Profile 模板或 IPv6 编辑。

WebUI 不自动安装 Linux/VPP 路由。实际 memif 用户会话的双向 ICMP、TCP/UDP
验证及 NAT、MTU、代理注意事项见 [后路由实测记录](framed-routes-live-validation.md)。

## 隔离回归测试

在仓库根目录执行。测试脚本仅接受名称为 `webui_framed_routes_test` 或以
`webui_framed_routes_test_` 开头且后缀为字母数字的数据库，运行结束会删除该测试库。
不要使用业务数据库连接串。

```bash
docker build --network host -f docker/webui/Dockerfile -t xcn-webui:framed-routes-test .
docker run -d --name webui-route-test-mongo \
  -p 127.0.0.1:27028:27017 mongo:latest --bind_ip_all
docker exec webui-route-test-mongo mongosh --quiet \
  --eval 'db.adminCommand({ping: 1})'
docker run --rm --network host --entrypoint node \
  -e WEBUI_TEST_DB_URI=mongodb://127.0.0.1:27028/webui_framed_routes_test \
  xcn-webui:framed-routes-test --test tests/framed-routes.js
docker rm -f webui-route-test-mongo
```

测试覆盖 CIDR 边界及非法输入、最多 8 条、旧数据兼容、API 新增/回读/修改/清空、
无关字段编辑保留路由，以及切片和 DNN 之间的配置隔离。

## 本次发布

- 新镜像：`localhost:5000/xcn-webui:ipv4-framed-routes-20261014`。
- 原镜像：`xcn-webui:latest`，保留用于回滚。
- 仅更新 `xcn/xcn-webui` Deployment，核心网和 MongoDB 配置保持原状。
- 本次基于原 WebUI 镜像复用依赖，已核对 `package.json` 和 `package-lock.json`
  与源码完全一致，并重新运行生产构建。
- 镜像摘要：`sha256:e43e3b450f6c20d5fc078ed15c4d38341bea99dd339b4f6a21a1a0df0917c537`。

后续 Helm 升级需在原有部署参数基础上保留以下镜像参数，避免恢复旧 WebUI：

```yaml
images:
  webui:
    repository: localhost:5000/xcn-webui
    tag: ipv4-framed-routes-20261014
```

仅回滚本次 WebUI 更新：

```bash
kubectl set image -n xcn deployment/xcn-webui webui=xcn-webui:latest
kubectl rollout status -n xcn deployment/xcn-webui --timeout=120s
```

## 验证记录（2026-10-14）

- 生产构建成功，7 项模型/API 回归测试全部通过。
- 隔离环境 Chromium 实测登录、已有路由回显、非法输入提示、`/0` 和 `/32`、
  8 条上限、增删及保存刷新、详情展示、清空、新建用户携带路由；无 JavaScript 错误。
- 现网 `xcn-webui` 更新后为 `1/1 Ready`，启动日志正常；实际 9999 页面登录成功，
  用户 `460110000000003` 的 `SST=1 / SD=010101 / cmnet` 下出现路由配置列表。
  此次现网页面检查阻止所有订阅写请求，没有给该业务用户添加测试路由。
- 发布前后全部 800 个订阅文档的 SHA256 摘要一致；`xcn-5gc`、`xcn-core`、
  `xcn-mongodb` 的 Pod UID 和容器重启次数均未改变，所有容器保持就绪。
