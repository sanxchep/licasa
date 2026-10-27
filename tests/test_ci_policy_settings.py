#!/usr/bin/env python3
"""Hosted policy plans preserve stronger controls and never guess unavailable settings."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import yaml

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from ci_policy_settings import build_plan
from check_ci_inputs import check_workflows
from build_ci_dependencies import command_for

class HostedSettingsTests(unittest.TestCase):
    def setUp(self):
        self.settings=json.loads((ROOT/'packaging/ci-policy-settings.json').read_text())
        self.snapshot={'repository':'sanxchep/licasa','source_sha':'a'*40,'protection':{},'environment':{},
            'checks':[{'name':name,'conclusion':'success','head_sha':'a'*40,
                       'app':{'slug':'github-actions','id':15368}} for name in self.settings['required_checks']]}

    def test_reviewable_plan_binds_real_check_producers_and_has_no_self_review_deadlock(self):
        plan=build_plan(self.settings,self.snapshot,'a'*40)
        self.assertEqual(plan['source_sha'],'a'*40)
        protection=plan['operations'][0]['body']
        self.assertEqual(protection['required_pull_request_reviews']['required_approving_review_count'],0)
        self.assertEqual({c['app_id'] for c in protection['required_status_checks']['checks']},{15368})
        self.assertTrue(protection['required_status_checks']['strict'])
        self.assertFalse(protection['allow_force_pushes']);self.assertFalse(protection['allow_deletions'])
        env=next(row['body'] for row in plan['operations'] if row['path'].startswith('/environments/'))
        self.assertFalse(env['prevent_self_review']);self.assertNotIn('can_admins_bypass',env)
        self.assertTrue(plan['manual_steps'])
        self.assertEqual(env['reviewers'],[{'type':'User','id':32956082}])

    def test_existing_stronger_reviews_checks_and_timers_are_preserved(self):
        self.snapshot['protection']={'required_pull_request_reviews':{'required_approving_review_count':2},
            'required_status_checks':{'checks':[{'context':'Existing security check','app_id':10}]}}
        self.snapshot['environment']={'protection_rules':[{'type':'wait_timer','wait_timer':60},
            {'type':'required_reviewers','prevent_self_review':True,
             'reviewers':[{'type':'User','reviewer':{'id':32956082}}]}]}
        plan=build_plan(self.settings,self.snapshot,'a'*40)
        self.assertEqual(plan['operations'][0]['body']['required_pull_request_reviews']['required_approving_review_count'],2)
        env=next(row['body'] for row in plan['operations'] if row['path'].startswith('/environments/'))
        self.assertTrue(env['prevent_self_review']);self.assertEqual(env['wait_timer'],60)
        self.assertEqual(len(plan['operations'][0]['body']['required_status_checks']['checks']),3)

    def test_inaccessible_unknown_or_widened_protections_require_reconciliation(self):
        mutations=[lambda s:s.update(protection=None),lambda s:s.update(environment=None),
                   lambda s:s.update(source_sha='b'*40),lambda s:s.update(checks=[]),
                   lambda s:s['checks'][0].update(conclusion='skipped'),
                   lambda s:s['protection'].update(restrictions={'users':[{'login':'owner'}]}),
                   lambda s:s['environment'].update(deployment_branch_policy={'protected_branches':True}),
                   lambda s:s['environment'].update(protection_rules=[{'type':'custom'}]),
                   lambda s:s['environment'].update(protection_rules=[{'type':'required_reviewers',
                       'reviewers':[{'type':'User','reviewer':{'id':7}}]}])]
        for mutate in mutations:
            state=copy.deepcopy(self.snapshot);mutate(state)
            with self.subTest(state=state),self.assertRaises(ValueError):build_plan(self.settings,state,'a'*40)

    def test_hosted_skip_rule_rejects_github_skip_markers(self):
        import re
        pattern=self.settings['prohibited_commit_message_pattern']
        for message in ('[skip ci]','[ci skip]','[no ci]','[skip actions]','[actions skip]',
                        'ordinary title\n\nskip-checks: true'):
            self.assertRegex(message,pattern)
        self.assertIsNone(re.search(pattern,'Fix save ownership'))
        plan=build_plan(self.settings,self.snapshot,'a'*40)
        rule=next(op['body'] for op in plan['operations'] if op['body'].get('name')=='Main commits must run CI')
        self.assertTrue(rule['rules'][0]['parameters']['negate'])

    def test_apply_requires_exact_reviewed_plan_and_unchanged_snapshot(self):
        from unittest.mock import patch
        import ci_policy_settings as module
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);snapshot=root/'snapshot.json';planfile=root/'plan.json'
            snapshot.write_text(json.dumps(self.snapshot))
            plan=build_plan(self.settings,self.snapshot,'a'*40);planfile.write_text(json.dumps(plan))
            args=['ci_policy_settings.py','apply','--source-sha','a'*40,'--snapshot',str(snapshot),
                  '--plan',str(planfile),'--approve-plan-sha256','wrong']
            with patch.object(sys,'argv',args),patch.object(module,'api') as api:
                with self.assertRaises(ValueError):module.main()
                api.assert_not_called()
            args[-1]=module.digest(plan)
            changed={**self.snapshot,'source_sha':'b'*40}
            with patch.object(sys,'argv',args),patch.object(module,'inspect',return_value=changed),patch.object(module,'api') as api:
                with self.assertRaises(ValueError):module.main()
                api.assert_not_called()
            with patch.object(sys,'argv',args),patch.object(module,'inspect',return_value=self.snapshot),patch.object(module,'api') as api:
                module.main()
                self.assertEqual(api.call_count,len(plan['operations']))

class ProfileWorkflowTests(unittest.TestCase):
    def test_matching_media_backend_and_full_shipped_feature_flags_are_required(self):
        flow=yaml.load((ROOT/'.github/workflows/verify-source.yml').read_text(),Loader=yaml.BaseLoader)
        modern='\n'.join(step.get('run','') for step in flow['jobs']['test']['steps'])
        for feature in ('HEIF','JXL','RAW','AVIF','APNG','JP2','MOTION_PLAYBACK'):
            self.assertIn('-DLICASA_ENABLE_'+feature+'=ON',modern)
        self.assertIn('LICASA_QT_FFMPEG_PLUGIN',modern)
        self.assertIn('all_qmllint',modern)
        for preset,sanitizer in (('asan','address'),('ubsan','undefined')):
            cmd=command_for('ffmpeg',preset,3)
            self.assertEqual(cmd[cmd.index('--sanitizer')+1],sanitizer)
            self.assertIn(str(ROOT/'build/deps'/preset),cmd)
        self.assertNotIn('--sanitizer',command_for('ffmpeg','release',3))
        self.assertEqual(set(flow['jobs']['result']['needs']),{'plan','quick','core','test'})

    def test_cold_runs_bypass_every_source_and_native_binary_cache(self):
        for name in ('verify-source.yml','build-native.yml'):
            flow=yaml.load((ROOT/'.github/workflows'/name).read_text(),Loader=yaml.BaseLoader)
            for job in flow['jobs'].values():
                for step in job.get('steps',[]):
                    if step.get('uses','').startswith('actions/cache@'):
                        self.assertIn('!inputs.cold',step.get('if',''))
            if name=='build-native.yml':
                builder=next(s for s in flow['jobs']['build']['steps'] if s.get('id')=='builder')
                self.assertIn('!inputs.cold',builder['with']['cache-from'])

    def test_queue_schema_and_cancel_combination_fail_closed(self):
        for queue,cancel,valid in (('max','false',True),('single','true',True),('bad','false',False),('max','true',False)):
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory);(root/'.github/workflows').mkdir(parents=True)
                (root/'.github/workflows/fixture.yml').write_text('name: fixture\non: push\nconcurrency:\n  group: publication\n  queue: '+queue+'\n  cancel-in-progress: '+cancel+'\njobs:\n  test:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: true\n')
                if valid:check_workflows(root)
                else:
                    with self.assertRaises(ValueError):check_workflows(root)


class ColdComparisonTests(unittest.TestCase):
    def test_comparison_distinguishes_missing_reference_different_bytes_and_wrong_source(self):
        from ci_reproducibility import compare
        current={'source_sha':'a'*40,'manifest_sha256':'b'*64,'version':'0.9.0',
                 'build_run_id':'12','packages':{'snap':{'path':'same.snap','sha256':'c'*64}}}
        self.assertEqual(compare(current,None)['status'],'unavailable')
        previous=copy.deepcopy(current);previous['build_run_id']='11'
        self.assertEqual(compare(current,previous)['status'],'identical')
        previous['packages']['snap']['sha256']='d'*64
        self.assertEqual(compare(current,previous)['differences'],['snap'])
        previous['source_sha']='e'*40
        with self.assertRaises(ValueError):compare(current,previous)

    def test_cross_host_artifact_redirect_drops_authorization(self):
        from ci_reproducibility import ArtifactRedirect
        import urllib.request
        request=urllib.request.Request('https://api.github.com/repos/a/b/actions/artifacts/1/zip',
                                       headers={'Authorization':'Bearer private-test-token'})
        redirected=ArtifactRedirect().redirect_request(request,None,302,'Found',{},'https://signed-artifact.example.invalid/file')
        self.assertIsNone(redirected.get_header('Authorization'))


class PinnedLinterTests(unittest.TestCase):
    def test_offline_verified_payload_and_corruption_or_link_are_checked(self):
        import hashlib,io,tarfile
        from prepare_ci_tools import prepare
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);payload=b'reviewed executable fixture';archive=root/'fixture.tar.gz'
            with tarfile.open(archive,'w:gz') as bundle:
                member=tarfile.TarInfo('actionlint');member.size=len(payload)
                bundle.addfile(member,io.BytesIO(payload))
            spec={'url':'https://example.invalid/fixture.tar.gz',
                  'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
                  'binary_sha256':hashlib.sha256(payload).hexdigest()}
            self.assertEqual(prepare(root,spec,offline=True).read_bytes(),payload)
            executable=root/'actionlint';executable.unlink();executable.symlink_to('unowned-file')
            with self.assertRaises(ValueError):prepare(root,spec,offline=True)
            executable.unlink();spec['binary_sha256']='0'*64
            with self.assertRaises(ValueError):prepare(root,spec,offline=True)

if __name__=='__main__':unittest.main()
