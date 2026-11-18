#!/usr/bin/env python3
"""Capture both real cores, exit them, then resume in fresh processes (POSIX)."""
import argparse
import hashlib
from pathlib import Path
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def run(args, stage, mode, frames=1200, delay=0):
    work = args.work_dir.resolve()
    endpoints = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    jobs = []
    try:
        for i, role in enumerate(('host', 'client')):
            if i and delay:
                time.sleep(delay)
            system = work / role / 'system'
            assets = system / 'PPSSPP'
            assets.mkdir(parents=True, exist_ok=True)
            for source in (ROOT / 'assets').iterdir():
                target = assets / source.name
                if source.name != 'netplay' and not target.exists():
                    target.symlink_to(source, target_is_directory=source.is_dir())
            saves = work / role / 'saves'
            saves.mkdir(exist_ok=True)
            log = (work / f'{stage}-{role}.log').open('wb')
            program = ROOT / 'pspautotests/tests/net/adhoc/pair' / f'gamemode_{role}.prx'
            endpoints[i].setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            command = [str(args.harness.resolve()), str(args.core.resolve()), str(program),
                       str(system), str(saves), 'pair', str(i), str(endpoints[i].fileno()),
                       mode[i] if isinstance(mode, tuple) else mode, str(frames)]
            jobs.append((subprocess.Popen(command, pass_fds=(endpoints[i].fileno(),),
                                          stdout=log, stderr=subprocess.STDOUT), log))
        for job, log in jobs:
            assert job.wait(timeout=max(40, frames / 60 + 20)) == 0, f'{stage}: core failed; see {work}'
            log.close()
    finally:
        for job, log in jobs:
            if job.poll() is None:
                job.kill()
                job.wait()
            log.close()
        for endpoint in endpoints:
            endpoint.close()
    return [(work / f'{stage}-{role}.log').read_text(errors='replace') for role in ('host', 'client')]


def successful(logs, stage):
    for i, log in enumerate(logs):
        for expected in ('NelfePlay checkpoint committed', 'Replica updates in 60 polls:',
                         "Peer's counter only increases:", 'PASS pair frontend'):
            assert expected in log, f'{stage}/player{i+1}: missing {expected}'
        assert 'Emulation remains stopped' not in log, f'{stage}/player{i+1}: checkpoint failed'
        # printf is split across log fragments; normalize without joining unrelated diagnostics.
        import re
        fragments = re.findall(r'^\[sceIo\] (.*?)(?=^\[|^PASS pair|\Z)', log, re.M | re.S)
        output = ''.join(fragment.removesuffix('\n') for fragment in fragments)
        assert '40 or more' in output, f'{stage}/player{i+1}: packets did not resume'
        assert "Peer's counter only increases: yes, magic right, timestamps increase" in output
        assert 'ExitGameMode: 00000000' in output
    print(f'PASS {stage}: both PSPs exchanged GameMode updates after the barrier', flush=True)


def main(args):
    logs = run(args, 'capture', 'capture')
    successful(logs, 'capture')
    originals, installed = [], []
    for i, role in enumerate(('host', 'client')):
        folder = args.work_dir.resolve() / role / 'system/PPSSPP/netplay'
        captures = list((folder / 'captures').glob(f'*-player{i+1}.state'))
        assert len(captures) == 1
        raw = captures[0].read_bytes()
        assert raw[:8] == b'NLFPSSP1'
        assert struct.unpack_from('<II', raw, 16) == (i, 2)
        assert hashlib.sha256(raw[:136] + raw[168:]).digest() == raw[136:168]
        path = folder / (captures[0].name.rsplit('-player', 1)[0] + '.state')
        originals.append(raw)
        installed.append(path)
        path.write_bytes(raw)
    assert originals[0][24:40] == originals[1][24:40], 'capture identity differs'
    assert originals[0] != originals[1], 'players must have separate states'
    successful(run(args, 'resume', 'resume'), 'fresh-process resume')
    successful(run(args, 'late-client', 'resume', delay=2), 'late-client resume')

    def rejected(name, data, reason, modes='resume', frames=240):
        for path, original in zip(installed, originals):
            path.write_bytes(original)
        if data is None:
            installed[1].unlink()
        else:
            installed[1].write_bytes(data)
        logs = run(args, name, modes, frames=frames)
        assert reason in '\n'.join(logs), f'{name}: expected {reason}'
        for log in logs:
            assert 'NelfePlay checkpoint committed' not in log, name
            assert 'Emulation remains stopped' in log, name
            assert 'Running for 3 seconds:' not in log, name
        print(f'PASS {name}: neither player resumed', flush=True)

    def changed(offset, length=1):
        data = bytearray(originals[1])
        data[offset:offset+length] = bytes(b ^ 1 for b in data[offset:offset+length])
        data[136:168] = hashlib.sha256(data[:136] + data[168:]).digest()
        return data

    rejected('wrong-player', originals[0], 'wrong player')
    rejected('legacy-layout', b'RASTATE\0' + bytes(200), 'Legacy checkpoint')
    rejected('wrong-checkpoint', changed(24), 'different checkpoints')
    rejected('wrong-build', changed(40), 'producing build')
    rejected('wrong-content', changed(104), 'game content differs')
    rejected('wrong-options', changed(120), 'options')
    rejected('corrupt-data', originals[1][:-1] + bytes([originals[1][-1] ^ 1]), 'Checkpoint hash')
    rejected('missing-state', None, 'checkpoint missing')
    rejected('disabled-start', originals[1], 'did not enable challenge start', ('resume', 'normal'))
    rejected('disconnect', originals[1], 'Player disconnected during checkpoint', 'resume-disconnect')
    if args.timeout_case:
        rejected('missing-player-timeout', originals[1], 'Checkpoint timed out', 'resume-missing', frames=3700)
    for path, original in zip(installed, originals):
        path.write_bytes(original)
    print('Checkpoint capture, fresh-process restoration and rejection cases passed.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('core', type=Path)
    parser.add_argument('--harness', type=Path, default=Path('build-netpacket-tests/NetpacketCoreSmoke'))
    parser.add_argument('--work-dir', type=Path, default=Path('build-netpacket-tests/checkpoint-results'))
    parser.add_argument('--timeout-case', action='store_true', help='also exercise the real 60-second deadline')
    main(parser.parse_args())
