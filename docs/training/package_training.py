#!/usr/bin/env python3
"""Build an offline diagram index and a portable training ZIP."""
from pathlib import Path
from html import escape
import json
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parent
STEM = '5G通信组网与运维培训'


def package():
    cards = []
    for spec in sorted((ROOT / 'assets').glob('[0-9][0-9]-*.json')):
        data = json.loads(spec.read_text())
        if 'diagram_type' not in data:
            continue
        stem = spec.stem
        title = escape(data['meta']['title'])
        cards.append(f'<article><a href="assets/{stem}.html"><img src="assets/{stem}.png" alt="{title}" loading="lazy"><h2>{stem[:2]} · {title}</h2></a><p><a href="assets/{stem}.svg">SVG</a> · <a href="assets/{stem}.png">PNG</a> · <a href="assets/{stem}.json">图源 JSON</a></p></article>')
    html = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>5G 培训 · 离线图册</title><style>
body{margin:0;background:#eef3f7;color:#19384d;font-family:system-ui,"Noto Sans CJK SC","Microsoft YaHei",sans-serif}main{max-width:1200px;margin:auto;padding:42px 24px}h1{font-size:30px;margin:0 0 14px}p{line-height:1.8}nav{margin:24px 0 32px}a{color:#17627e;text-decoration:none}a:hover{text-decoration:underline}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:22px}article{padding:18px;border:1px solid #cad7e2;border-radius:8px;background:white}article img{width:100%;height:220px;object-fit:contain}h2{font-size:17px;line-height:1.5}article p{font-size:13px;margin-bottom:0}footer{margin-top:36px;font-size:13px}
</style><main><h1>5G 通信组网 · 离线图册</h1><p>选择图表打开交互版。原生图支持缩放、主题切换及补充说明；上行按主箭头，下行及 IP 变化请结合讲义中的报文表阅读。</p><nav><a href="5G通信组网与运维培训.docx">Word 讲义</a> · <a href="5G通信组网与运维培训.pdf">PDF 讲义</a> · <a href="5G通信组网与运维培训.md">Markdown 源稿</a></nav><section class="grid">'''
    html += '\n'.join(cards) + '</section><footer>V1.0 · 环境快照 2026-10-25 · 15 张图 · 教学地址与运行快照在讲义中分别注明</footer></main></html>\n'
    (ROOT / '图表索引.html').write_text(html)
    archive = ROOT / (STEM + '-资料包.zip')
    with ZipFile(archive, 'w', ZIP_DEFLATED, compresslevel=9) as z:
        for file in sorted(ROOT.rglob('*')):
            if not file.is_file() or file.suffix in ('.zip', '.pyc') or '__pycache__' in file.parts:
                continue
            if any(part.startswith('.') for part in file.relative_to(ROOT).parts):
                continue
            z.write(file, Path(STEM) / file.relative_to(ROOT))
    with ZipFile(archive) as z:
        assert z.testzip() is None
        print(f'{archive}: {len(z.namelist())} files, {archive.stat().st_size} bytes')


if __name__ == '__main__':
    package()
