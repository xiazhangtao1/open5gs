#!/usr/bin/env python3
"""Refresh the Word TOC and render that DOCX to PDF with LibreOffice/UNO.

Ubuntu: sudo apt install libreoffice-writer python3-uno fonts-noto-cjk
Run with the system Python (where python3-uno is installed).
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import time
import uuid
import uno
from com.sun.star.beans import PropertyValue


def prop(name, value):
    p = PropertyValue()
    p.Name, p.Value = name, value
    return p


def render(source, output):
    output.mkdir(parents=True, exist_ok=True)
    pipe = 'training_' + uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix='training-lo-') as profile:
        process = subprocess.Popen([
            'soffice', '-env:UserInstallation=' + Path(profile).as_uri(),
            '--headless', '--norestore', '--nodefault', '--nofirststartwizard',
            '--accept=pipe,name=' + pipe + ';urp;StarOffice.ComponentContext',
        ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        document = desktop = None
        try:
            local = uno.getComponentContext()
            resolver = local.ServiceManager.createInstanceWithContext(
                'com.sun.star.bridge.UnoUrlResolver', local)
            for _ in range(100):
                try:
                    context = resolver.resolve(
                        'uno:pipe,name=' + pipe + ';urp;StarOffice.ComponentContext')
                    break
                except Exception:
                    if process.poll() is not None:
                        raise RuntimeError(process.stderr.read().decode())
                    time.sleep(.2)
            else:
                raise TimeoutError('LibreOffice did not start')
            desktop = context.ServiceManager.createInstanceWithContext(
                'com.sun.star.frame.Desktop', context)
            document = desktop.loadComponentFromURL(source.as_uri(), '_blank', 0,
                (prop('Hidden', True), prop('ReadOnly', False)))
            if document is None:
                raise RuntimeError('Failed to open DOCX')
            document.refresh()
            indexes = document.getDocumentIndexes()
            if indexes.getCount() != 1:
                raise RuntimeError(f'Expected one TOC, found {indexes.getCount()}')
            for i in range(indexes.getCount()):
                indexes.getByIndex(i).update()
            document.refresh()
            document.storeAsURL((output / source.name).as_uri(), (
                prop('FilterName', 'Office Open XML Text'), prop('Overwrite', True)))
            document.storeToURL((output / source.with_suffix('.pdf').name).as_uri(), (
                prop('FilterName', 'writer_pdf_Export'), prop('Overwrite', True)))
            print('Updated TOC and rendered:', output / source.name)
        finally:
            if document is not None:
                document.close(True)
            if desktop is not None:
                desktop.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('docx', type=Path)
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    render(args.docx.resolve(), args.output_directory.resolve())
