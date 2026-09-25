"""Run deterministic tests against original/patched SDK code, without editing SDK."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from prepare_idf_tls import ROOT, patched_source, verified_source

def extract(source):
    text=source.decode('utf-8')
    start=text.index('static int esp_tls_low_level_conn(')
    end=text.index('\n}\n',start)+3
    return text[start:end]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf-path',type=Path,default=os.environ.get('IDF_PATH'))
    a=p.parse_args()
    if not a.idf_path:p.error('Set IDF_PATH or use --idf-path')
    original=verified_source(a.idf_path)
    patched=patched_source(original)
    with tempfile.TemporaryDirectory(prefix='cf-sdk-test-') as directory:
        temp=Path(directory)
        for name,data in [('baseline',original),('patched',patched)]:
            (temp/(name+'.inc')).write_text(extract(data),encoding='utf-8')
        subprocess.run(['cmake','-S',str(ROOT/'tests/idf_tls'),'-B',str(temp/'build'),'-D',f'SDK_EXTRACT_DIR={temp}'],check=True)
        subprocess.run(['cmake','--build',str(temp/'build'),'--config','Debug'],check=True)
        subprocess.run(['ctest','--test-dir',str(temp/'build'),'-C','Debug','--output-on-failure','-V'],check=True)

if __name__=='__main__':main()
