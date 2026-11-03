#!/usr/bin/env python3
"""Do players' DS resume points still load? Run by own_os_release.py before it packages anything.

tests/resume-fixtures/ds holds a resume point from each format and release, all from our own ROM. The engine loads
each and plays on, without a screen. On a Mac it runs in the Lima VM the images are built in.

    tools/resume_check.py                 check the built engine against every fixture
    tools/resume_check.py --add 0.8.0     also keep what this engine writes, for every later build to read
"""
import argparse, gzip, os, platform, shlex, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/resume-fixtures/ds'
ROM = FIXTURES / 'Impeccable DS.nds'
ENGINE = ROOT / 'build/engines/rgds-experimental/rgds-experimental'
LIMA = ROOT / '.cache/linux-builder'
CONFIG = '[emu]\nautosave = true\n[paths]\nstates = {states}\nsaves = {states}\n'


def in_linux(command, cwd):
    """The handheld's engine needs aarch64 Linux; the VM sees this repository at the same path."""
    shell = 'cd ' + shlex.quote(str(cwd)) + ' && SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software ' \
        'SDL_AUDIODRIVER=dummy timeout 120 ' + shlex.join(str(part) for part in command)
    if platform.system() == 'Linux' and platform.machine() == 'aarch64':
        return subprocess.run(['sh', '-c', shell], capture_output=True, text=True, errors='replace')
    environment = dict(os.environ, LIMA_HOME=str(LIMA / 'state'))
    return subprocess.run([LIMA / 'lima/bin/limactl', 'shell', 'rgds-image', '--', 'sh', '-c', shell],
                          capture_output=True, text=True, errors='replace', env=environment)


def play(engine, work, *arguments):
    # no JIT: this checks the format, and the VM's CPU is not the handheld's
    command = [engine, ROM, '--config', work / 'config.ini', '--interp', '--bios7', '', '--bios9', '', '--firmware', '']
    return in_linux(command + list(arguments), work)


def loads(engine, work, fixture):
    state = work / 'resume.dss'
    with gzip.open(fixture) as stream:
        state.write_bytes(stream.read())
    result = play(engine, work, '--load-state', state, '--frames', '120')
    if result.returncode == 0 and 'state: loaded ' in result.stderr: return None
    lines = [line for line in result.stderr.splitlines() if line.startswith('state:')]
    return f'exit {result.returncode}: ' + (lines[-1] if lines else result.stderr[-200:])


def add(engine, work, name):
    result = play(engine, work, '--frames', '300')
    written = list((work / 'states').glob('*.auto.dss'))
    if result.returncode or not written: raise SystemExit(f'No resume point was written:\n{result.stderr[-500:]}')
    with gzip.open(FIXTURES / (name + '.dss.gz'), 'wb', 9) as out:
        out.write(written[0].read_bytes())


def check(engine, name=None):
    """The failures, as (fixture, reason)."""
    # inside the repository: the VM mounts it, not the Mac's temporary folder
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temp:
        work = Path(temp)
        (work / 'states').mkdir()
        (work / 'config.ini').write_text(CONFIG.format(states=work / 'states'))
        if name: add(engine, work, name)
        failed = []
        for fixture in sorted(FIXTURES.glob('*.dss.gz')):
            reason = loads(engine, work, fixture)
            if reason: failed.append((fixture.name, reason))
        return failed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--engine', type=Path, default=ENGINE)
    parser.add_argument('--add', metavar='NAME', help='keep what this engine writes as a fixture')
    args = parser.parse_args()
    if not args.engine.is_file(): raise SystemExit(f'{args.engine} is not built.')
    fixtures = len(list(FIXTURES.glob('*.dss.gz')))
    failed = check(args.engine.resolve(), args.add)
    for fixture, reason in failed:
        print(f'{fixture}: {reason}', file=sys.stderr)
    if failed:
        raise SystemExit('This build would lose DS resume points. Fix the state format; see docs/DEVELOPMENT.md "Saves".')
    print(f'DS resume points load: {fixtures + bool(args.add)} of them.')


if __name__ == '__main__':
    main()
