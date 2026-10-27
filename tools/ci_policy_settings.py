#!/usr/bin/env python3
"""Inspect and prepare reviewable hosted CI protections; apply only a reviewed plan.

The local implementation never calls apply. Deployment requires published source,
real successful check producers and an explicit approval of the exact JSON plan.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import urllib.request

ROOT=Path(__file__).resolve().parents[1]


def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def api(repository, path, *, method='GET', body=None):
    if not re.fullmatch(r'[\w.-]+/[\w.-]+',repository) or not path.startswith('/') or '..' in path:
        raise ValueError('Unsafe API target')
    token=os.environ.get('GH_TOKEN','')
    if not token:raise ValueError('Hosted administration requires an explicitly supplied GH_TOKEN')
    request=urllib.request.Request('https://api.github.com/repos/'+repository+path,method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={'Accept':'application/vnd.github+json','Authorization':'Bearer '+token,
                 'X-GitHub-Api-Version':'2026-03-10','Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=30) as response:
        data=response.read()
        return json.loads(data) if data else None


def build_plan(settings, snapshot, source):
    if not re.fullmatch('[0-9a-f]{40}',source) or snapshot.get('source_sha')!=source:
        raise ValueError('Inspect the exact reviewed and deployed main source first')
    if snapshot.get('repository')!=settings['repository']:
        raise ValueError('Wrong repository settings snapshot')
    if snapshot.get('protection') is None or snapshot.get('environment') is None:
        raise ValueError('Unavailable settings cannot be overwritten by assumed defaults')
    checks=snapshot.get('checks',[])
    producers=[]
    for name in settings['required_checks']:
        matches=[c for c in checks if c.get('name')==name and c.get('conclusion')=='success'
                 and c.get('app',{}).get('slug')=='github-actions' and c.get('head_sha')==source]
        if len(matches)!=1:raise ValueError('Require one successful real check producer: '+name)
        producers.append({'context':name,'app_id':matches[0]['app']['id']})
    old=snapshot['protection'];previous=old.get('required_status_checks') or {}
    known={c['context']:c for c in previous.get('checks',[])}
    for context in previous.get('contexts',[]):known.setdefault(context,{'context':context,'app_id':None})
    for check in producers:
        if check['context'] in known and known[check['context']].get('app_id') not in (None,check['app_id']):
            raise ValueError('Existing check producer differs; explicit reconciliation required')
        known[check['context']]=check
    reviews=old.get('required_pull_request_reviews') or {}
    if old.get('lock_branch',{}).get('enabled',False):
        raise ValueError('Locked main requires explicit manual reconciliation')
    for field in ('dismissal_restrictions','bypass_pull_request_allowances'):
        value=reviews.get(field) or {}
        if any(value.get(key) for key in ('users','teams','apps')):
            raise ValueError('Existing review actor restrictions require manual preservation')
        if field in reviews:reviews={k:v for k,v in reviews.items() if k!=field}
    review_fields={'dismissal_restrictions','dismiss_stale_reviews','require_code_owner_reviews',
                   'required_approving_review_count','require_last_push_approval','bypass_pull_request_allowances'}
    protection={
        'required_status_checks':{'strict':True,'checks':list(known.values())},
        'enforce_admins':True,
        'required_pull_request_reviews':{**{k:v for k,v in reviews.items() if k in review_fields},
             'required_approving_review_count':max(reviews.get('required_approving_review_count',0),0)},
        'restrictions':old.get('restrictions'),
        'required_linear_history':old.get('required_linear_history',{}).get('enabled',False),
        'allow_force_pushes':False,'allow_deletions':False,
        'block_creations':old.get('block_creations',{}).get('enabled',False),
        'required_conversation_resolution':True}
    if protection['restrictions'] is not None:
        raise ValueError('Existing actor restrictions require manual preservation; no automatic overwrite')
    env=snapshot['environment']
    if env.get('deployment_branch_policy') is not None:
        raise ValueError('Existing environment branch/tag restrictions require manual reconciliation')
    rules=env.get('protection_rules',[])
    if any(r.get('type') not in {'required_reviewers','wait_timer'} for r in rules):
        raise ValueError('Unknown stronger environment protection; refusing replacement')
    reviewers=[];prevent=settings['publication_environment']['prevent_self_review'];wait=0
    for rule in rules:
        if rule['type']=='required_reviewers':
            reviewers=[{'type':r['type'],'id':r['reviewer']['id']} for r in rule.get('reviewers',[])]
            prevent=prevent or rule.get('prevent_self_review',False)
        elif rule['type']=='wait_timer':wait=rule.get('wait_timer',0)
    owner={'type':'User','id':settings['publication_environment']['reviewer_id']}
    if reviewers and owner not in reviewers:
        raise ValueError('Adding a new eligible approver would widen existing environment access')
    if not reviewers:reviewers.append(owner)
    operations=[
        {'method':'PUT','path':'/branches/main/protection','body':protection},
        {'method':'PUT','path':'/actions/permissions/workflow','body':settings['workflow_permissions']},
        {'method':'PUT','path':'/actions/permissions/fork-pr-contributor-approval',
         'body':{'approval_policy':settings['fork_approval']}},
        {'method':'PUT','path':'/environments/snap-beta','body':{'wait_timer':wait,'reviewers':reviewers,
             'prevent_self_review':prevent,'deployment_branch_policy':None}},
        {'method':'POST','path':'/rulesets','body':{'name':'Immutable Licasa version tags','target':'tag',
          'enforcement':'active','bypass_actors':[],
          'conditions':{'ref_name':{'include':['refs/tags/v*'],'exclude':[]}},
          'rules':[{'type':'update'},{'type':'deletion'}]}}]
    if snapshot.get('tag_ruleset') is not None:
        rule=snapshot['tag_ruleset']
        if (rule.get('target')!='tag' or rule.get('enforcement')!='active' or rule.get('bypass_actors')
                or rule.get('conditions',{}).get('ref_name',{}).get('include')!=['refs/tags/v*']
                or not {'update','deletion'} <= {r['type'] for r in rule.get('rules',[])}):
            raise ValueError('Existing version-tag ruleset needs manual reconciliation')
        operations.pop()  # Never replace a previously administered tag ruleset implicitly.
    skip_rule={'name':'Main commits must run CI','target':'branch','enforcement':'active',
        'bypass_actors':[], 'conditions':{'ref_name':{'include':['refs/heads/main'],'exclude':[]}},
        'rules':[{'type':'commit_message_pattern','parameters':{'name':'No CI skip instructions',
            'operator':'regex','negate':True,'pattern':settings['prohibited_commit_message_pattern']}}]}
    if snapshot.get('skip_ruleset') is None:
        operations.append({'method':'POST','path':'/rulesets','body':skip_rule})
    else:
        existing=snapshot['skip_ruleset']
        if any(existing.get(key)!=value for key,value in skip_rule.items()):
            raise ValueError('Existing main skip-instruction rule needs manual reconciliation')
    return {'schema_version':1,'repository':settings['repository'],'source_sha':source,
            'snapshot_sha256':digest(snapshot),'operations':operations,
            'manual_steps':['Disable administrator bypass for snap-beta in GitHub UI; the documented REST environment update schema does not expose that setting. Verify repository artifact retention permits 90 days.']}


def inspect(settings,source):
    repo=settings['repository'];main=api(repo,'/commits/main')
    if main['sha']!=source:raise ValueError('Reviewed source is not deployed at main')
    checks=api(repo,f'/commits/{source}/check-runs?per_page=100')
    if checks['total_count']!=len(checks['check_runs']):raise ValueError('Incomplete check producer inventory')
    rules=api(repo,'/rulesets?per_page=100')
    if len(rules)==100:raise ValueError('Incomplete ruleset inventory')
    matches=[r for r in rules if r['name']=='Immutable Licasa version tags']
    if len(matches)>1:raise ValueError('Ambiguous existing version-tag rulesets')
    tag_rule=api(repo,f"/rulesets/{matches[0]['id']}") if matches else None
    skip_matches=[r for r in rules if r['name']=='Main commits must run CI']
    if len(skip_matches)>1:raise ValueError('Ambiguous skip-instruction rulesets')
    skip_rule=api(repo,f"/rulesets/{skip_matches[0]['id']}") if skip_matches else None
    # 404/403 are deliberately not interpreted as empty protection settings.
    return {'repository':repo,'source_sha':source,'protection':api(repo,'/branches/main/protection'),
            'environment':api(repo,'/environments/snap-beta'),'checks':checks['check_runs'],
            'tag_ruleset':tag_rule,'skip_ruleset':skip_rule}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('inspect','plan','apply'))
    parser.add_argument('--source-sha',required=True)
    parser.add_argument('--snapshot',type=Path,required=True)
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--approve-plan-sha256')
    args=parser.parse_args();settings=json.loads((ROOT/'packaging/ci-policy-settings.json').read_text())
    if args.action=='inspect':
        args.snapshot.write_text(json.dumps(inspect(settings,args.source_sha),indent=2)+'\n');return
    snapshot=json.loads(args.snapshot.read_text());plan=build_plan(settings,snapshot,args.source_sha)
    if args.action=='plan':
        args.plan.write_text(json.dumps(plan,indent=2)+'\n');print('Review plan SHA-256:',digest(plan));return
    saved=json.loads(args.plan.read_text())
    if saved!=plan or args.approve_plan_sha256!=digest(plan):raise ValueError('Approve the exact complete generated plan first')
    fresh=inspect(settings,args.source_sha)
    if digest(fresh)!=plan['snapshot_sha256']:raise ValueError('Hosted settings changed since review; re-plan')
    for operation in plan['operations']:
        api(plan['repository'],operation['path'],method=operation['method'],body=operation['body'])
    print('Applied reviewed API protections; inspect again and complete the manual UI steps before activation')


if __name__=='__main__':main()
