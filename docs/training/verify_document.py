#!/usr/bin/env python3
"""Check content preservation, embedded images, TOC and rendered page bounds."""
import json
from pathlib import Path
import re
from zipfile import ZipFile
import xml.etree.ElementTree as ET
from markdown_it import MarkdownIt
import pymupdf

ROOT = Path(__file__).resolve().parent
STEM = '5G通信组网与运维培训'
NS = {'w': 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}


def normalize(text):
    return re.sub(r'\s+', '', text)


def verify():
    with ZipFile(ROOT / (STEM + '.docx')) as z:
        document = ET.fromstring(z.read('word/document.xml'))
        content = normalize(''.join(e.text or '' for e in document.findall('.//w:t', NS)))
        images = [n for n in z.namelist() if n.startswith('word/media/')]
        assert len(images) == 15, images
        fields = ''.join(e.text or '' for e in document.findall('.//w:instrText', NS))
        assert 'TOC' in fields
    source = (ROOT / (STEM + '.md')).read_text()
    tokens = MarkdownIt('commonmark').enable('table').parse(source)
    checked = 0
    missing = []
    for token in tokens:
        if token.type == 'fence':
            text = token.content
        elif token.type == 'inline':
            text = ''.join(t.content for t in token.children
                           if t.type in ('text', 'code_inline', 'image'))
        else:
            continue
        if normalize(text) not in content:
            missing.append(text[:100])
        checked += 1
    assert not missing, missing
    pdf = pymupdf.open(ROOT / (STEM + '.pdf'))
    bounds = []
    for i, page in enumerate(pdf):
        for block in page.get_text('dict')['blocks']:
            for line in block.get('lines', []):
                for span in line['spans']:
                    x0, y0, x1, y1 = span['bbox']
                    if x0 < -1 or y0 < -1 or x1 > page.rect.width + 1 or y1 > page.rect.height + 1:
                        bounds.append((i + 1, span['text']))
        assert '\ufffd' not in page.get_text(), i + 1
    assert not bounds, bounds
    assert 'NFVI' in pdf[1].get_text() and '附录 D' in pdf[1].get_text()
    assert not re.search(r'VPP\s*抓包|trace\s+add|pcap\s+trace', content, re.I)
    result = {'status': 'pass', 'pages': len(pdf), 'embedded_images': len(images),
              'source_segments_preserved': checked, 'toc_populated': True,
              'page_bound_violations': bounds, 'vpp_capture_section': 'removed'}
    (ROOT / 'validation' / 'document.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    verify()
