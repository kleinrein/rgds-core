#!/usr/bin/env python3
"""Format the repository's Python (yapf) and C/C++ (clang-format).

The formatter versions are pinned in requirements-format.txt and installed on
first use into .cache/format-tools, so every machine produces the same layout.
Style lives in .style.yapf, .clang-format and dscore/.clang-format; the files
left alone are listed in .yapfignore and .clang-format-ignore.

  python3 tools/format.py              rewrite every tracked file in place
  python3 tools/format.py --check      list files that need formatting, exit 1 if any
  python3 tools/format.py --diff       print what would change, touch nothing
  python3 tools/format.py --changed    only files changed against HEAD, plus new ones
  python3 tools/format.py --cpp src/   only C/C++, only under src/
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import difflib
import fnmatch
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import venv

ROOT = Path(__file__).resolve().parents[1]
REQUIREMENTS = ROOT / 'requirements-format.txt'
TOOLS = ROOT / '.cache' / 'format-tools'
PYTHON_SUFFIXES = {'.py'}
CPP_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'}


def tool(name):
    folder = 'Scripts' if os.name == 'nt' else 'bin'
    return TOOLS / folder / (name + ('.exe' if os.name == 'nt' else ''))


def ensure_tools():
    """Install the pinned formatters once, and again whenever the pins change."""
    pins = hashlib.sha256(REQUIREMENTS.read_bytes()).hexdigest()
    stamp = TOOLS / 'requirements.sha256'
    if stamp.is_file() and stamp.read_text().strip() == pins:
        return
    print(f'Installing formatters from {REQUIREMENTS.name} into {TOOLS.relative_to(ROOT)} ...', file=sys.stderr)
    venv.create(TOOLS, with_pip=True, clear=True)
    pip = [str(tool('python')), '-m', 'pip', 'install', '--quiet', '--disable-pip-version-check']
    subprocess.run(pip + ['-r', str(REQUIREMENTS)], check=True)
    stamp.write_text(pins + '\n')


def ignore_patterns(name):
    """Patterns from an ignore file, relative to the repository root."""
    path = ROOT / name
    if not path.is_file():
        return []
    lines = (line.strip() for line in path.read_text().splitlines())
    return [line for line in lines if line and not line.startswith('#')]


def ignored(path, patterns):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def git_files(*args):
    output = subprocess.run(['git', *args, '-z'], cwd=ROOT, check=True, capture_output=True).stdout
    return [name for name in output.decode().split('\0') if name]


def candidates(changed):
    if changed:
        names = git_files('diff', '--name-only', '--diff-filter=d', 'HEAD')
    else:
        names = git_files('ls-files', '--cached')
    names += git_files('ls-files', '--others', '--exclude-standard')
    return sorted({name for name in names if (ROOT / name).is_file() and not (ROOT / name).is_symlink()})


def under(name, roots):
    return not roots or any(name == root or name.startswith(root.rstrip('/') + '/') for root in roots)


def formatter(name):
    suffix = Path(name).suffix
    if suffix in PYTHON_SUFFIXES:
        return [str(tool('yapf')), '--style', str(ROOT / '.style.yapf')]
    return [str(tool('clang-format')), '--style=file', '--assume-filename', name]


def format_one(name):
    """(name, original, formatted, error) for one file; the file itself is not touched."""
    path = ROOT / name
    original = path.read_bytes()
    result = subprocess.run(formatter(name), cwd=ROOT, input=original, capture_output=True)
    if result.returncode != 0:
        return name, original, None, result.stderr.decode(errors='replace').strip()
    if original.strip() and not result.stdout.strip():
        return name, original, None, 'formatter returned no output'
    return name, original, result.stdout, None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='list files that need formatting; exit 1 if any do')
    mode.add_argument('--diff', action='store_true', help='print a unified diff instead of rewriting files')
    language = parser.add_mutually_exclusive_group()
    language.add_argument('--python', action='store_true', help='only Python files (yapf)')
    language.add_argument('--cpp', action='store_true', help='only C/C++ files (clang-format)')
    parser.add_argument('--changed', action='store_true', help='only files changed against HEAD, plus untracked ones')
    parser.add_argument('paths', nargs='*', help='limit to these files or directories (repository-relative)')
    args = parser.parse_args(argv)

    roots = []
    for path in args.paths:
        resolved = Path(path).resolve()
        try:
            relative = resolved.relative_to(ROOT).as_posix()
        except ValueError:
            parser.error(f'{path} is outside the repository')
        if relative == '.':
            roots = []
            break
        roots.append(relative)
    suffixes = set()
    if not args.cpp:
        suffixes |= PYTHON_SUFFIXES
    if not args.python:
        suffixes |= CPP_SUFFIXES
    python_ignored = ignore_patterns('.yapfignore')
    cpp_ignored = ignore_patterns('.clang-format-ignore')
    names = []
    for name in candidates(args.changed):
        suffix = Path(name).suffix
        if suffix not in suffixes or not under(name, roots):
            continue
        if ignored(name, python_ignored if suffix in PYTHON_SUFFIXES else cpp_ignored):
            continue
        names.append(name)
    if not names:
        print('No files to format.')
        return 0

    ensure_tools()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(format_one, names))

    errors = [(name, error) for name, _, _, error in results if error]
    pending = [
        (name, original, formatted) for name, original, formatted, error in results
        if not error and formatted != original
    ]
    for name, error in errors:
        print(f'error: {name}: {error}', file=sys.stderr)

    if args.check:
        for name, _, _ in pending:
            print(name)
        print(f'{len(pending)} of {len(names)} files need formatting.', file=sys.stderr)
    elif args.diff:
        for name, original, formatted in pending:
            before = original.decode(errors='replace').splitlines(keepends=True)
            after = formatted.decode(errors='replace').splitlines(keepends=True)
            sys.stdout.writelines(difflib.unified_diff(before, after, 'a/' + name, 'b/' + name))
    else:
        skipped = 0
        for name, original, formatted in pending:
            path = ROOT / name
            # Another editor may have saved this file while it was being formatted; keep their version.
            if path.read_bytes() != original:
                print(f'skipped {name}: changed while formatting, run again', file=sys.stderr)
                skipped += 1
                continue
            path.write_bytes(formatted)
        print(f'Formatted {len(pending) - skipped} of {len(names)} files.', file=sys.stderr)
    if errors:
        return 2
    return 1 if pending and (args.check or args.diff) else 0


if __name__ == '__main__':
    sys.exit(main())
