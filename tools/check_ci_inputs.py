#!/usr/bin/env python3
"""Check public prose and executable CI inputs without contacting external sites."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check_docs(root=ROOT):
    for path in (root / 'README.md',):
        text = path.read_text()
        if any(line.rstrip() != line for line in text.splitlines()):
            raise ValueError(f'Trailing whitespace: {path.name}')
        for target in re.findall(r'!?\[[^\]]*\]\(([^\s)]+)(?:\s+"[^"]*")?\)', text):
            if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith('#'):
                continue
            # GitHub branch/tree links and public URL anchors are not local file paths.
            local = target.split('#', 1)[0]
            if local and not (path.parent / local).exists():
                raise ValueError(f'Missing README link target: {target}')


def check_workflows(root=ROOT):
    import yaml
    workflows = {p.name: yaml.load(p.read_text(), Loader=yaml.BaseLoader)
                 for p in (root / '.github/workflows').glob('*.yml')}
    for name, workflow in workflows.items():
        concurrency = workflow.get('concurrency', {})
        if isinstance(concurrency, dict) and 'queue' in concurrency:
            if concurrency['queue'] not in {'single', 'max'}:
                raise ValueError(f'Invalid concurrency queue: {name}')
            if concurrency['queue'] == 'max' and concurrency.get('cancel-in-progress', 'false') != 'false':
                raise ValueError(f'Queued transactions cannot cancel in-progress work: {name}')
        if not workflow.get('on') or not workflow.get('jobs'):
            raise ValueError(f'Missing workflow triggers/jobs: {name}')
        for job_id, job in workflow['jobs'].items():
            needs = job.get('needs', [])
            needs = [needs] if isinstance(needs, str) else needs
            if set(needs) - workflow['jobs'].keys():
                raise ValueError(f'Unknown needs: {name}/{job_id}')
            references = set(re.findall(r'\bneeds\.([a-zA-Z_][a-zA-Z_0-9]*)\.', str(job)))
            if references - set(needs):
                raise ValueError(f'Output without direct dependency: {name}/{job_id}')
            for step in job.get('steps', []):
                script = step.get('run')
                shell = step.get('shell', 'bash')
                if script and shell.startswith('bash'):
                    # Expressions are values supplied by Actions before bash parses the block.
                    script = re.sub(r'\$\{\{.*?\}\}', 'CI_EXPRESSION', script)
                    subprocess.run(['bash', '-n'], input=script, text=True, check=True)
    for directory in ('tools', 'packaging'):
        for path in sorted((root / directory).rglob('*.sh')):
            subprocess.run(['bash', '-n', str(path)], check=True)
    print(f'Validated {len(workflows)} workflow graphs and shell inputs')


def actionlint(executable, root=ROOT):
    # The installed upstream linter predates GitHub's documented queue field.
    # Validate that field above, then lint every other byte/Actions expression.
    check_workflows(root)
    with tempfile.TemporaryDirectory() as temporary:
        target = Path(temporary) / '.github/workflows'
        target.mkdir(parents=True)
        for path in (root / '.github/workflows').glob('*.yml'):
            text = path.read_text()
            match = re.search(r'(?m)^concurrency:\n(?:(?:  .*|)\n)*', text)
            if match:
                block = re.sub(r'(?m)^  queue: (?:single|max)\n', '', match.group())
                text = text[:match.start()] + block + text[match.end():]
            (target / path.name).write_text(text)
        subprocess.run([str(Path(executable).resolve()), '-shellcheck=', '-pyflakes=',
                        *map(str, sorted(target.glob('*.yml')))], cwd=temporary, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--docs', action='store_true')
    parser.add_argument('--actionlint', help='Also run the supplied pinned local Actions linter')
    args = parser.parse_args()
    check_docs()
    if not args.docs:
        check_workflows()
    if args.actionlint:
        actionlint(args.actionlint)


if __name__ == '__main__':
    main()
