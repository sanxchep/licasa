#!/usr/bin/env python3
"""Exercise public-repository events and source-bound completion receipts."""
import copy
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from ci_events import NIGHTLY,WEEKLY,changes_for,docs_only,event_profile,open_pull_request,enforce_coverage
from ci_plan import make_plan,check_results
from ci_receipt import create,validate
JOBS={'plan','public_files','package_contract','rpm_preflight','test','ubuntu','debian','wrappers','snap','packages','snap_desktops','attest'}

def pr(action='synchronize',draft=False):
    return {'action':action,'pull_request':{'draft':draft,'base':{'ref':'main'}}}

class Events(unittest.TestCase):
    def test_every_main_tip_is_full(self):
        for payload in ({},{'commits':[{'message':'docs'}]},{'forced':True},{'commits':[{},{}]},{'sender':{'type':'Bot'}}):
            self.assertEqual(event_profile('push','refs/heads/main',payload,requested='FAST',open_pr=True)['profile'],'FULL')

    def test_branch_dedup_api_failure_and_deletion(self):
        for lookup,expected in ((True,'NONE'),(False,'FAST'),(None,'FAST')):
            self.assertEqual(event_profile('push','refs/heads/work',{},open_pr=lookup)['profile'],expected)
        self.assertEqual(event_profile('push','refs/heads/work',{'deleted':True})['profile'],'NONE')
        with patch('ci_events.urllib.request.urlopen',side_effect=urllib.error.URLError('offline')):
            self.assertIsNone(open_pull_request('owner/repo','work','test-token'))

    def test_ready_draft_fork_and_dependabot_have_identical_coverage(self):
        for action in ('opened','synchronize','reopened','ready_for_review','converted_to_draft'):
            for draft in (False,True):
                for origin in ('internal','fork','dependabot'):
                    payload=pr(action,draft);payload['origin']=origin
                    result=event_profile('pull_request','refs/pull/7/merge',payload)
                    self.assertEqual(result['profile'],'FAST' if draft else 'FULL')
                    self.assertEqual(result['merge_eligible'],str(not draft).lower())

    def test_metadata_retarget_queue_schedules_manual_and_tags(self):
        for action in ('closed','edited'):
            self.assertEqual(event_profile('pull_request','refs/pull/7/merge',pr(action))['profile'],'NONE')
        payload=pr('edited');payload['changes']={'base':{'ref':{'from':'old'}}}
        self.assertEqual(event_profile('pull_request','refs/pull/7/merge',payload)['profile'],'FULL')
        group={'action':'checks_requested','merge_group':{'base_ref':'refs/heads/main'}}
        self.assertEqual(event_profile('merge_group','queue',group)['profile'],'FULL')
        for schedule,cold in ((NIGHTLY,'false'),(WEEKLY,'true')):
            result=event_profile('schedule','refs/heads/main',{'schedule':schedule})
            self.assertEqual((result['profile'],result['cold']),('FULL',cold))
        for choice in ('FULL','FAST'):
            self.assertEqual(event_profile('workflow_dispatch','main',{},requested=choice)['profile'],choice)
        self.assertEqual(event_profile('push','refs/tags/v0.9.0',{})['profile'],'FULL')
        self.assertEqual(event_profile('push','refs/tags/notes',{})['profile'],'NONE')
        for args in (('push','refs/tags/v0.9.0',{'forced':True}),('push','refs/tags/vbad',{}),
                     ('schedule','main',{'schedule':'unknown'}),('merge_group','queue',{'action':'destroyed'})):
            with self.assertRaises(ValueError):event_profile(*args)
        for event in ('release','repository_dispatch','workflow_run','issue_comment','create'):
            with self.assertRaises(ValueError):event_profile(event,'main',{})

    def test_docs_are_modified_allowlisted_prose_only(self):
        row={'path':'README.md','status':'M','patch':'+Clearer introduction'}
        self.assertTrue(docs_only([row]))
        for changed in ({'path':'packaging/release-notes/v0.9.0.md'},{'status':'A'},{'status':'D'},
                        {'status':'R100'},{'patch':''},{'patch':'+Supported Ubuntu 26.04'},
                        {'path':'.github/workflows/readme.md'},{'path':'tools/policy.md'}):
            self.assertFalse(docs_only([{**row,**changed}]),changed)
        self.assertFalse(docs_only([row,{'path':'qml/Main.qml','status':'M','patch':'change'}]))
        self.assertFalse(docs_only([]))

    def test_pr_cannot_remove_or_weaken_its_own_target_contracts(self):
        original={'artifacts':{'deb':'deb/pkg_{version}.deb'},'targets':[
            {'id':'native','artifact':'deb','profile':'qt','status':'supported','qualifier':['strict.py']}]}
        enforce_coverage(original,copy.deepcopy(original))
        for mutate in (lambda d:d.update(targets=[]),
                       lambda d:d['targets'][0].update(qualifier=['weaker.py']),
                       lambda d:d['targets'][0].update(status='unsupported'),
                       lambda d:d['artifacts'].update(deb='other_{version}.deb')):
            candidate=copy.deepcopy(original);mutate(candidate)
            with self.assertRaises(ValueError):enforce_coverage(original,candidate)

    def test_full_diff_cli_and_wrong_manual_sha(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);root=work/'source';root.mkdir()
            def git(*args):return subprocess.check_output(['git',*args],cwd=root,text=True,stderr=subprocess.DEVNULL).strip()
            git('init','-b','main');git('config','user.email','fixture@example.invalid');git('config','user.name','Fixture')
            (root/'README.md').write_text('Introduction\n')
            (root/'CMakeLists.txt').write_text('project(Licasa VERSION 0.9.0 LANGUAGES CXX)\n')
            (root/'snap').mkdir();(root/'snap/snapcraft.yaml').write_text("version: '0.9.0'\n")
            (root/'packaging').mkdir();(root/'packaging/support-matrix.json').write_bytes((ROOT/'packaging/support-matrix.json').read_bytes())
            git('add','.');git('commit','-qm','base');base=git('rev-parse','HEAD')
            (root/'README.md').write_text('Clearer introduction\n');git('commit','-qam','prose');source=git('rev-parse','HEAD')
            self.assertTrue(docs_only(changes_for(root,base,source)))
            payload=pr();payload['number']=7;payload['pull_request']['base']['sha']=base
            event=work/'event';event.write_text(json.dumps(payload));output=work/'output';receipt=work/'receipt'
            env={**os.environ,'GITHUB_EVENT_PATH':str(event),'GITHUB_EVENT_NAME':'pull_request',
                 'GITHUB_REF':'refs/pull/7/merge','SOURCE_SHA':source,'POLICY_SHA':base,'GITHUB_REPOSITORY':'owner/repo',
                 'GITHUB_RUN_ID':'12','GITHUB_RUN_ATTEMPT':'1'}
            cmd=[sys.executable,'-B',str(ROOT/'tools/ci_events.py'),'--root',str(root),'--output',str(output),'--receipt',str(receipt)]
            result=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads(receipt.read_text())['profile'],'DOCS')
            (root/'runtime.py').write_text('print("changed")\n');git('add','.');git('commit','-qm','runtime')
            (root/'README.md').write_text('Another introduction\n');git('commit','-qam','more prose');source=git('rev-parse','HEAD')
            self.assertFalse(docs_only(changes_for(root,base,source)))
            env.update(SOURCE_SHA=source,GITHUB_EVENT_NAME='workflow_dispatch',EXPECTED_SHA='a'*40)
            self.assertNotEqual(subprocess.run(cmd,env=env,capture_output=True,timeout=10).returncode,0)

class WorkflowPolicyArchive(unittest.TestCase):
    def test_reviewed_archive_routes_candidate_and_rejects_missing_completion_gate(self):
        flow=yaml.load((ROOT/'.github/workflows/ci.yml').read_text(),Loader=yaml.BaseLoader)
        def script(job,prefix):
            return next(step['run'] for step in flow['jobs'][job]['steps']
                        if step.get('name','').startswith(prefix))
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);root=work/'source';root.mkdir();(root/'tools').mkdir()
            for name in ('ci_events','ci_plan','ci_receipt','check_support_manifest','hosted_candidate','runtime_profiles'):
                shutil.copy2(ROOT/'tools'/(name+'.py'),root/'tools'/(name+'.py'))
            (root/'packaging').mkdir();shutil.copy2(ROOT/'packaging/support-matrix.json',root/'packaging/support-matrix.json')
            (root/'CMakeLists.txt').write_text('project(Licasa VERSION 0.9.0 LANGUAGES CXX)\n')
            (root/'snap').mkdir();(root/'snap/snapcraft.yaml').write_text("version: '0.9.0'\n")
            def git(*args):
                return subprocess.check_output(['git',*args],cwd=root,text=True,stderr=subprocess.DEVNULL).strip()
            git('init','-b','main');git('config','user.email','fixture@example.invalid');git('config','user.name','Fixture')
            git('add','.');git('commit','-qm','reviewed policy');policy=git('rev-parse','HEAD')
            (root/'tools/ci_events.py').write_text('raise RuntimeError("candidate policy must not execute")\n')
            git('commit','-qam','proposed policy');source=git('rev-parse','HEAD')
            event=work/'event.json';payload=pr();payload['number']=7;payload['pull_request']['base']['sha']=policy
            event.write_text(json.dumps(payload));runner=work/'runner'
            env={**os.environ,'RUNNER_TEMP':str(runner),'RUNNER_ENVIRONMENT':'github-hosted','POLICY_SHA':policy,
                 'SOURCE_SHA':source,'GITHUB_EVENT_PATH':str(event),'GITHUB_EVENT_NAME':'pull_request',
                 'GITHUB_REF':'refs/pull/7/merge','GITHUB_REPOSITORY':'owner/repo','GITHUB_RUN_ID':'12',
                 'GITHUB_RUN_ATTEMPT':'1','GITHUB_OUTPUT':str(work/'outputs'),'PROFILE':'FULL',
                 'REUSABLE':'false','COLD':'false','EXPECTED_SHA':'','GH_TOKEN':''}
            def execute(command,cwd=root):
                return subprocess.run(['bash','-e','-o','pipefail','-c',command],cwd=cwd,env=env,
                                      capture_output=True,text=True,timeout=20)
            for command in (script('plan','Load the reviewed policy'),script('plan','Select exact coverage')):
                result=execute(command);self.assertEqual(result.returncode,0,result.stderr)
            plan=json.loads((runner/'verification-plan.json').read_text())
            self.assertEqual((plan['sha'],plan['policy_sha'],plan['profile'],plan['merge_eligible']),
                             (source,policy,'FULL','true'))
            self.assertEqual(git('status','--porcelain'),'')
            policy_root=runner/'ci-policy';destination=policy_root/'build/verification-result';destination.mkdir(parents=True)
            (destination/'verification-plan.json').write_text(json.dumps(plan))
            needs={name:{'result':'success'} for name in JOBS};needs['plan']['outputs']=plan
            env['CI_RESULTS']=json.dumps(needs)
            for command in (script('result','Reject failed'),script('result','Record complete')):
                result=execute(command,policy_root);self.assertEqual(result.returncode,0,result.stderr)
            receipt=json.loads((destination/'verification-result.json').read_text())
            validate(receipt,repository='owner/repo',source=source,version='0.9.0',run_id='12',attempt='1')
            del needs['rpm_preflight'];env['CI_RESULTS']=json.dumps(needs)
            for command in (script('result','Reject failed'),script('result','Record complete')):
                result=execute(command,policy_root);self.assertNotEqual(result.returncode,0)
                self.assertIn('rpm_preflight',result.stderr)

class Receipts(unittest.TestCase):
    def setUp(self):
        self.plan={**make_plan('push',True,[]),'sha':'a'*40,'policy_sha':'b'*40,'version':'0.9.0',
                   'run_id':'12','run_attempt':'1','merge_eligible':'true'}
        self.needs={name:{'result':'success'} for name in JOBS};self.needs['plan']['outputs']=self.plan

    def test_every_missing_cancelled_skipped_failed_gate_rejects(self):
        self.assertEqual(set(check_results(self.needs)),JOBS)
        for name in JOBS:
            for status in ('failure','cancelled','skipped',None):
                rows=copy.deepcopy(self.needs)
                if status is None:rows.pop(name)
                else:rows[name]['result']=status
                with self.subTest(name=name,status=status),self.assertRaises(ValueError):check_results(rows)
        for key in ('profile','build','packages','complete','presets'):
            rows=copy.deepcopy(self.needs);rows['plan']['outputs'].pop(key)
            with self.assertRaises(ValueError):check_results(rows)
        rows=copy.deepcopy(self.needs);rows['plan']['outputs'].update(make_plan('pull_request',False,[],profile='FAST'))
        with self.assertRaises(ValueError):check_results(rows)

    def test_only_exact_full_source_run_attempt_receipt_qualifies_release(self):
        receipt=create(self.plan,self.needs,'owner/repo','12','1')
        args={'repository':'owner/repo','source':'a'*40,'version':'0.9.0','run_id':'12','attempt':'1'}
        validate(receipt,**args)
        for key,value in (('profile','FAST'),('source_sha','c'*40),('run_attempt','2'),('run_id','13'),('repository','evil/repo')):
            changed=copy.deepcopy(receipt);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):validate(changed,**args)
        changed=copy.deepcopy(receipt);changed['outcomes'].pop('rpm_preflight')
        with self.assertRaises(ValueError):validate(changed,**args)

    def test_metadata_edits_preserve_inflight_checks_and_retargets_replace_them(self):
        concurrency=yaml.load((ROOT/'.github/workflows/ci.yml').read_text(),Loader=yaml.BaseLoader)['concurrency']
        def actual(event='pull_request',action='synchronize',base=False,run='100',ref='refs/pull/7/merge'):
            values={'github.event_name':event,'github.event.action':action,'github.event.changes.base':base,
                    'github.event.pull_request.number':7,'github.workflow':'Verify','github.run_id':run,'github.ref':ref}
            def evaluate(match):
                expression=re.sub(r'github\.[a-zA-Z_.]+',lambda token:repr(values[token.group()]),match.group(1))
                return eval(expression.replace('&&',' and ').replace('||',' or '),
                            {'__builtins__':{}},{'startsWith':lambda text,prefix:text.startswith(prefix)})
            group=re.sub(r'\$\{\{(.*?)\}\}',lambda match:str(evaluate(match)),concurrency['group'])
            cancel=evaluate(re.fullmatch(r'\$\{\{(.*?)\}\}',concurrency['cancel-in-progress']))
            return group,bool(cancel)
        running,cancel=actual();self.assertTrue(cancel)
        for action in ('opened','synchronize','ready_for_review','converted_to_draft','closed'):
            self.assertEqual(actual(action=action,run='101'),(running,True))
        self.assertEqual(actual(action='edited',base=True,run='102'),(running,True))
        metadata,cancel=actual(action='edited',run='103')
        self.assertNotEqual(metadata,running);self.assertFalse(cancel)
        self.assertNotEqual(metadata,actual(action='edited',run='104')[0])
        main,cancel=actual(event='push',ref='refs/heads/main');self.assertFalse(cancel)
        self.assertNotEqual(main,actual(event='push',ref='refs/heads/main',run='101')[0])
        branch,cancel=actual(event='push',ref='refs/heads/work');self.assertTrue(cancel)
        self.assertEqual(branch,actual(event='push',ref='refs/heads/work',run='101')[0])

    def test_trigger_and_permission_contract(self):
        d=yaml.load((ROOT/'.github/workflows/ci.yml').read_text(),Loader=yaml.BaseLoader)
        self.assertEqual(set(d['on']),{'push','pull_request','merge_group','schedule','workflow_dispatch','workflow_call'})
        self.assertNotIn('paths-ignore',d['on']['push']);self.assertNotIn('release',d['on'])
        self.assertIn('github.run_id',d['concurrency']['group'])
        self.assertIn("github.ref != 'refs/heads/main'",d['concurrency']['cancel-in-progress'])
        self.assertEqual(d['permissions'],{'contents':'read'})
        self.assertEqual(d['on']['workflow_dispatch']['inputs']['profile']['default'],'FULL')
        self.assertEqual({x['cron'] for x in d['on']['schedule']},{NIGHTLY,WEEKLY})
        for job in d['jobs'].values():
            self.assertNotIn('environment',job);self.assertNotIn('write',job.get('permissions',{}).values())

if __name__=='__main__':unittest.main()
