# X-CN 镜像构建与离线交付

支持 Linux/amd64。三个自有镜像均基于 Ubuntu 22.04；monitor 包含采集、agent 和信令解码功能，部署仍使用五类镜像。

## 基础镜像与 APT

内网需提供以下基础镜像，本地存在时直接使用，缺失时从指定 Harbor 拉取：

```text
192.168.10.35/xnet/ubuntu:22.04
192.168.10.35/xcn/vpp:26.06
```

也可提前 `docker save`，运入内网后 `docker load`。Kubernetes 的容器运行时需能获取部署镜像；仅 Docker 导入不代表 containerd 已导入。MongoDB 是部署依赖，需另行预载，或通过 `images.mongodb.repository/tag` 指向内网已有镜像。

`--offline` 使用已提交的 `runtime/sources.list`：`http://192.168.10.36/ubuntu_22_04/` 下的 jammy、jammy-updates、jammy-backports、jammy-security 及四个组件。不依赖代理，不回退外网。APT 包由内网软件源提供；联网模式保留 Ubuntu 公共源。

## 构建并自动推送

在仓库根目录运行，tag 由操作者指定：

```bash
./docker/build-images.sh --tag 26.10.27 --offline

# 仅构建并推送一个镜像；--image 可选 runtime、webui、monitor、all，默认 all
./docker/build-images.sh --tag 26.10.27 --image runtime --offline
./docker/build-images.sh --tag 26.10.27 --image webui --offline
./docker/build-images.sh --tag 26.10.27 --image monitor --offline

# 业务主机：CPU 编号需按实际空闲核选择，避开 UPF/VPP 及其 SMT 同胞
./docker/build-images.sh --tag 26.10.27 --offline --cpuset 36,37

# 联网环境只有公共 Ubuntu 和本地 VPP 标签时，显式替换同版本基础镜像地址
UBUNTU_IMAGE=ubuntu:22.04 VPP_IMAGE=vpp:26.06 \
  ./docker/build-images.sh --tag 26.10.27 --cpuset 36,37
```

默认生成并逐一推送：

```text
192.168.10.35/xcn/xcn-runtime:<tag>
192.168.10.35/xcn/xcn-webui:<tag>
192.168.10.35/xcn/xcn-monitor:<tag>
```

指定 `--image` 时仅校验、构建并推送所选组件；只有 runtime（或 all）需要 VPP 基础镜像。单镜像构建仍支持 `--offline`、`--registry` 和 `--cpuset`，沿用相同的资源限制与推送失败处理。

`--registry localhost:5000/xcn` 可替换输出仓库前缀，基础镜像地址独立配置。需要认证时先执行 `docker login 192.168.10.35`。仓库 HTTP/TLS 配置由环境管理员维护，脚本不重启 Docker。

push 失败会提示并继续，本地镜像保留；所选镜像均构建成功时退出码为 0。恢复仓库连接后可直接执行 `docker push <镜像:tag>`。构建失败、所选组件的离线文件缺失或校验失败返回非零退出码。

构建依次执行，默认主机网络、两个编译作业、1.5 CPU 时间额度、4 GiB 内存。`--cpuset` 不传时不额外绑定 CPU；传入后限制构建容器及编译进程的运行 CPU，不调整线上核心网 CPU 配置。

可选环境变量：`UBUNTU_IMAGE`、`VPP_IMAGE`、`BUILD_JOBS`、`BUILD_MEMORY`、`BUILD_CPU_QUOTA`（周期 100000 微秒）、`PUSH_TIMEOUT`（秒）、`APT_MIRROR`（显式替换 APT 根地址）、`BUILD_NETWORK`（默认 host；隔离验证可指定临时 Docker 网络）。使用 Docker 本地构建器，确保预载镜像可用并落实 cgroup 限制；需要保留本地构建器支持的 Docker Engine。

## Git 中的离线材料

`vendor/manifest.json` 记录版本、来源、许可证、逐文件 SHA-256 和适用组件。`vendor/archives/` 保存全部非 APT 构建输入，无 Git LFS 或在线附件依赖；超过 64 MiB 的归档分卷保存。

- freeDiameter、prometheus-client-c 固定到已验证提交，不跟随可变分支。
- 固定 Node 19.9.0、Python 3.12.15、Wireshark 4.6.9、wait 2.9.0。
- npm 缓存来自当前 package-lock，只包含公开包及缓存索引；构建使用 `npm ci --offline` 并执行必要的安装脚本。
- Python wheels 针对 CPython 3.12、Linux amd64/Jammy；`monitor/requirements.lock` 固定整个依赖图和哈希，使用 `pip --no-index --require-hashes`。
- 许可证随源码、发行包、wheel dist-info 和 npm 包保留；wait 的 Apache-2.0 许可证单独归档并带入 WebUI 镜像。

检查交付完整性：

```bash
python3 docker/vendor.py verify all
python3 -m unittest discover -s docker/tests -v
```

正常构建不调用准备工具。修改 package.json、package-lock 或 Python 依赖后，在联网环境显式刷新并重新验证：

```bash
taskset -c 36,37 python3 docker/prepare-offline-deps.py
python3 docker/vendor.py verify all
```

准备工具需 curl、Git、Python 3/pip，自动使用固定版本的 Node/npm 生成缓存，不使用主机 node_modules。若本地 C/C++ 子项目有未归档补丁或提交不一致则停止，避免丢失改动。归档、manifest 和锁文件需一并提交 Git。

## 监控镜像 GLIBC 兼容性排查

`GLIBC_2.35 not found (required by /opt/python/lib/libpython3.12.so.1.0)` 表示容器实际加载的系统库无法满足 Python 的 ABI 要求。先核对容器镜像本身和挂载配置，不据此判断宿主机需要升级 GLIBC；默认监控 Pod 不挂载宿主机 `/lib` 或 `/usr/lib`。

当前 `monitor/Dockerfile` 的编译和运行阶段使用同一个 `UBUNTU_IMAGE`。最终阶段在生产 UID 下实际启动 Python、导入扩展、加载原生 trace reader，并执行 TShark；不兼容将导致构建失败，不进入自动 push。

在发生问题的节点核对基础镜像内容和最终镜像（第二条中的镜像名、tag 按实际部署填写）：

```bash
docker run --rm --network none --entrypoint /bin/sh \
  192.168.10.35/xnet/ubuntu:22.04 \
  -c 'cat /etc/os-release; getconf GNU_LIBC_VERSION'

docker run --rm --network none --entrypoint /bin/sh \
  192.168.10.35/xcn/xcn-monitor:latest \
  -c 'cat /etc/os-release; getconf GNU_LIBC_VERSION; ldd /opt/python/lib/libpython3.12.so.1.0; python3 --version'

kubectl get pods -n xcn -l app=xcn-monitor \
  -o 'custom-columns=POD:.metadata.name,NODE:.spec.nodeName,IMAGES:.spec.containers[*].image,IMAGE_IDS:.status.containerStatuses[*].imageID'
```

预期基础系统为 Ubuntu 22.04，GLIBC 为 2.35，最终镜像中 Python 可运行。镜像标签只是名称，不能替代上述核对；还需确认实际执行的 Dockerfile、`UBUNTU_IMAGE` 覆盖值及构建日志。若 Docker 自检成功而 Pod 仍失败，核对 Pod 的实际 image ID、旧的 `collectorImage` 覆盖及库目录挂载。Docker 本地镜像和 Kubernetes containerd 缓存可能不同。

修正来源或构建配置后使用新 tag 重新构建和部署；仅覆盖 `latest` 再重建 Pod，在显式 `IfNotPresent` 配置下仍可能使用节点旧镜像。不要通过向容器或宿主机手工复制 `libc.so`、`libm.so` 处理该错误。

## 部署

Helm 默认 hostNetwork、memif、external_network VF，NAT44 关闭，AMF 完整/短网络名称均为 U6G。填写本环境 AMF、UPF N3、VPP N3/N6 地址和必要网关，不可使用测试地址。

```bash
helm upgrade --install xcn helm/xcn -n xcn --create-namespace \
  -f /path/to/site-values.yaml \
  --set-string images.runtime.tag=26.10.27 \
  --set-string images.webui.tag=26.10.27 \
  --set-string monitor.image.tag=26.10.27
```

旧环境若对 monitor collector/agent/decoder 设置过独立镜像覆盖，应同步更新或清空覆盖字段，让四个角色继承 `monitor.image`。运维工具、核心网未剥离的调试符号和容器角色保持不变。

关闭 NAT 时，外部网络须配置 UE 网段经 VPP N6 外部地址的回程路由；依赖 NAT 的环境显式设置 `vpp.n6.nat44.enabled=true`。升级前备份 Helm 值、镜像和终端流量配置，升级后重新接入测试终端并确认双向转发。
