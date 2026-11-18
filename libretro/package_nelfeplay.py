#!/usr/bin/env python3
"""Package a built Windows x64 core for manual testing, without installing it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dll', type=Path)
    parser.add_argument('--output', type=Path, default=Path('dist'))
    args = parser.parse_args()
    dll = args.dll.read_bytes()
    assert dll[:2] == b'MZ', 'Not a Windows PE file'
    pe = struct.unpack_from('<I', dll, 0x3c)[0]
    assert dll[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', dll, pe+4)[0] == 0x8664, 'Not x64'
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    source = f'https://github.com/Nelfe80/ppsspp/tree/{commit}'
    files = {
        'cores/ppsspp_netplay_libretro.dll': dll,
        'cores/ppsspp_netplay_license.txt': (
            f'Experimental NelfePlay PPSSPP core\nSource and submodules: {source}\n\n'
            + (ROOT / 'LICENSE.TXT').read_text(encoding='utf-8')).encode(),
        'info/ppsspp_netplay_libretro.info': (ROOT / 'libretro/ppsspp_netplay_libretro.info').read_bytes(),
        'README_NETPLAY.md': (ROOT / 'libretro/README_NETPLAY.md').read_text(encoding='utf-8').replace(
            '../docs/nelfeplay-netpacket-resume.md', 'docs/nelfeplay-netpacket-resume.md').encode(),
        'docs/nelfeplay-netpacket-resume.md': (ROOT / 'docs/nelfeplay-netpacket-resume.md').read_bytes(),
    }
    metadata = {'source_commit': commit, 'source_url': source, 'architecture': 'Windows x64',
                'protocol': 'ppsspp-nelfeplay-checkpoint-1', 'checkpoint_format': 'NLFPSSP1',
                'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    files['build.json'] = (json.dumps(metadata, indent=2) + '\n').encode()
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / 'PPSSPP-Netplay-RetroBat-x64-experimental.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for name, data in files.items():
            output.writestr(name, data)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n', encoding='ascii')
    print(archive)


if __name__ == '__main__':
    main()
