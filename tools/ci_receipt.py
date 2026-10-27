#!/usr/bin/env python3
"""Bind a completed CI gate to its exact source, plan, run and package evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from ci_plan import check_results


def plan_digest(plan: dict) -> str:
    return hashlib.sha256(json.dumps(plan, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def create(plan: dict, needs: dict, repository: str, run_id: str, attempt: str) -> dict:
    if (plan.get('run_id') != run_id or plan.get('run_attempt') != attempt
            or not re.fullmatch(r'[1-9][0-9]*', run_id)
            or not re.fullmatch(r'[1-9][0-9]*', attempt)
            or not re.fullmatch(r'[0-9a-f]{40}', plan.get('sha', ''))
            or not re.fullmatch(r'[0-9a-f]{40}', plan.get('policy_sha', ''))):
        raise ValueError('CI plan/run/source identity mismatch')
    outputs = needs.get('plan', {}).get('outputs', {})
    for key in ('sha', 'policy_sha', 'profile', 'complete', 'packages', 'build', 'presets', 'version',
                'matrix', 'merge_eligible', 'cold', 'retention', 'cache_scope'):
        if outputs.get(key) != plan.get(key):
            raise ValueError(f'Changed CI plan output: {key}')
    required = check_results(needs)
    return {'schema_version': 1, 'status': 'passed', 'repository': repository,
            'source_sha': plan['sha'], 'policy_sha': plan['policy_sha'],
            'run_id': run_id, 'run_attempt': attempt, 'profile': plan['profile'],
            'version': plan['version'], 'plan_sha256': plan_digest(plan),
            'required_jobs': required,
            'outcomes': {name: needs[name]['result'] for name in required}, 'plan': plan}


def validate(receipt: dict, *, repository: str, source: str, version: str,
             run_id: str, attempt: str) -> dict:
    expected = {'schema_version': 1, 'status': 'passed', 'repository': repository,
                'source_sha': source, 'version': version, 'run_id': run_id,
                'run_attempt': attempt, 'profile': 'FULL'}
    if any(receipt.get(key) != value for key, value in expected.items()):
        raise ValueError('Release requires a successful FULL receipt for this exact run/attempt/source')
    plan = receipt.get('plan', {})
    needs = {name: {'result': status} for name, status in receipt.get('outcomes', {}).items()}
    needs.setdefault('plan', {})['outputs'] = plan
    rebuilt = create(plan, needs, repository, run_id, attempt)
    if rebuilt != receipt:
        raise ValueError('Incomplete or modified verification receipt')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('create', 'check'))
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--receipt', type=Path)
    parser.add_argument('--source-sha')
    parser.add_argument('--version')
    parser.add_argument('--run-id', default=os.environ.get('GITHUB_RUN_ID', ''))
    parser.add_argument('--attempt', default=os.environ.get('GITHUB_RUN_ATTEMPT', ''))
    args = parser.parse_args()
    if args.action == 'create':
        result = create(json.loads(args.plan.read_text()), json.loads(os.environ['CI_RESULTS']),
                        os.environ['GITHUB_REPOSITORY'], args.run_id, args.attempt)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    else:
        validate(json.loads(args.receipt.read_text()), repository=os.environ['GITHUB_REPOSITORY'],
                 source=args.source_sha, version=args.version, run_id=args.run_id, attempt=args.attempt)
        print('Exact FULL verification receipt passed')


if __name__ == '__main__':
    main()
