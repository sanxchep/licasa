#!/usr/bin/env python3
"""Stage the checksum-pinned Actions linter in an explicit task/runner directory."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

from source_cache import fetch_archive

ROOT=Path(__file__).resolve().parents[1]


def prepare(output, spec, *, offline=False):
    archive=fetch_archive(output,spec['url'],spec['sha256'],offline=offline,limit=16*1024*1024)
    with tarfile.open(archive) as bundle:
        members=[m for m in bundle.getmembers() if m.name=='actionlint']
        if len(members)!=1 or not members[0].isfile() or members[0].size>32*1024*1024:
            raise ValueError('Invalid linter payload')
        binary=bundle.extractfile(members[0]).read()
    if hashlib.sha256(binary).hexdigest()!=spec['binary_sha256']:
        raise ValueError('Linter executable differs from reviewed binary')
    destination=output/'actionlint'
    if destination.is_symlink():
        raise ValueError('Refusing a linked linter destination')
    destination.write_bytes(binary)
    destination.chmod(0o755)
    return destination


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--offline',action='store_true')
    args=parser.parse_args()
    spec=json.loads((ROOT/'packaging/ci-tools.lock.json').read_text())['actionlint']
    print(prepare(args.output,spec,offline=args.offline))


if __name__=='__main__':main()
