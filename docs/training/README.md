# 培训资料使用说明

面向研发、测试与运维同事的三小时课程。内容以 2026-10-25 的 xnet-server 查询快照、当前 Open5GS 仓库与用户提供的自研代码片段为依据。

- **发给同事**：[Word 讲义](5G通信组网与运维培训.docx)，已嵌入 15 张图并生成目录、页码，可独立打开。
- **固定版式打印**：[PDF 讲义](5G通信组网与运维培训.pdf)，由最终 Word 同一轮排版导出。
- **维护内容**：[Markdown 源稿](5G通信组网与运维培训.md)，需与 `assets/` 一起保存。
- **课堂演示图表**：[离线图册](图表索引.html)，浏览器打开后选择图表，可缩放、切换主题和查看补充说明。
- **检查依据**：[验证记录](VALIDATION.md)；机器可读结果与命令日志在 `validation/`。

课程顺序为：服务器/NFVI → 容器与 Kubernetes → Linux 路由/NAT → 5G 组网 → 性能测试 → Kuboard/k9s。附录提供速查、部署参数、自研函数解释与来源。按要求不包含 VPP 抓包与 trace 教程。

资料包含三种硬件布线、核心网 TUN/双 memif、核心和边缘各自的 NAT/路由模式、模拟 UE 下挂 Wi-Fi 的桥接和 WAN/NAT 两类接法。报文表同时解释上行与下行 IP/端口变化。自研基站 N3 与边缘 DN 共用同一个用户面 VF；核心网的 N3/N6 使用两个 VF，勿混淆。

## 阅读与分发

Word 和 PDF 可以单独分发。使用 Markdown 或离线图册时，解压整个资料包并保留目录结构。JSON 是可编辑图源，HTML 是交互版，SVG/PNG 是适合讲义排版的打印衍生图。打印图放大了文字，并裁去部分空白和通用图例；正文提供完整图意。

Word 使用 Noto Sans CJK SC 字体；缺少该字体时 Word 会使用本地替代字体，分页可能略有变化。离线 HTML/SVG 也需要本地中文字体，通常可使用系统已有的微软雅黑、苹方或 Noto CJK；无中文字体的 Linux 可安装 `fonts-noto-cjk`。需要完全一致的打印效果请使用 PDF；PNG 图已包含字形。

## 重新生成 Word

以下在仓库根目录操作，仅生成文件，不修改集群：

```bash
python3 -m venv /tmp/xcn-training-venv
/tmp/xcn-training-venv/bin/pip install -r docs/training/requirements.txt
/tmp/xcn-training-venv/bin/python docs/training/build_document.py
```

生成器以 Markdown 为唯一正文来源，直接嵌入 `assets/*.png`。要填充目录并生成 PDF，需要系统安装 LibreOffice、python3-uno 和中文字体。Ubuntu 可使用：

```bash
sudo apt install libreoffice-writer python3-uno fonts-noto-cjk
/usr/bin/python3 docs/training/render_word.py \
  docs/training/5G通信组网与运维培训.docx /tmp/xcn-training-rendered
cp /tmp/xcn-training-rendered/5G通信组网与运维培训.docx docs/training/
cp /tmp/xcn-training-rendered/5G通信组网与运维培训.pdf docs/training/
/tmp/xcn-training-venv/bin/python docs/training/verify_document.py
```

本次实际使用临时容器中的 LibreOffice，未向业务主机安装这些排版依赖。`render_word.py` 为自身启动的 LibreOffice 分配独立配置目录，完成后关闭该实例。

## 更新图表

仅改正文无需重新生成图表。更新拓扑时先改 `build_diagrams.py`，再生成 JSON，使用已安装的 Archify 3.2.0 或兼容版本验证并渲染 HTML。例如：

```bash
python3 docs/training/build_diagrams.py
# 路径按本机安装位置调整
ARCHIFY_BIN=/home/xiazhangtao/.agents/skills/archify/bin/archify.mjs
node "$ARCHIFY_BIN" finalize architecture \
  docs/training/assets/07-core-memif.json \
  docs/training/assets/07-core-memif.html \
  --repo-root . --quality showcase --json \
  --out-dir /tmp/training-core-review-new
```

01–14 为 architecture，15 为 workflow；07 带仓库源码依据，需 `--repo-root .`。每次修改后使用新的检查输出目录，保留当前通过的检查记录。其他图用对应文件名逐一执行。浏览器检查需要可用的 Chromium，可通过 `ARCHIFY_CHROME` 指定路径。

打印导出需要 Node.js 的 `playwright` 模块与 Chromium；可用 `PLAYWRIGHT_MODULE` 指定已安装模块的绝对路径，用 `TRAINING_CJK_FONT` 指定本机中文字体文件。准备好后执行 `node docs/training/export_diagrams.cjs`，再重新生成 Word。该导出脚本不修改原生 HTML。

## 验证与打包

```bash
# Markdown 语法/图片与三种 Helm 模式检查，不访问集群
/tmp/xcn-training-venv/bin/python docs/training/validate_training.py
# 可选：增加回环地址上的 10 组低速 iperf 测试，需安装 iperf 与 iperf3
/tmp/xcn-training-venv/bin/python docs/training/validate_training.py --iperf
# 最终 Word/PDF 内容一致性与页边界检查
/tmp/xcn-training-venv/bin/python docs/training/verify_document.py
# 生成离线图册和完整 ZIP（不会把旧 ZIP 再装进去）
/tmp/xcn-training-venv/bin/python docs/training/package_training.py
```

这里的命令验证不替代商用终端、自研基站/UE、无线链路或 10G/100G 环境的端到端验收。教学地址、查询命令和会修改环境的实验命令已在正文区分。
