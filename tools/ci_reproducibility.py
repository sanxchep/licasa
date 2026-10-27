#!/usr/bin/env python3
"""Compare cold package outputs with a completed same-source main verification.

Only JSON is read from the remote evidence ZIP. No artifact code is extracted or
executed, and comparison never substitutes for this run's own qualification.
"""
import argparse
import io
import json
import os
from pathlib import Path
import re
import urllib.request
import urllib.parse
import zipfile

LIMIT=32*1024*1024


class ArtifactRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, target):
        redirect = super().redirect_request(request, response, code, message, headers, target)
        if redirect is not None and urllib.parse.urlsplit(request.full_url).netloc != urllib.parse.urlsplit(target).netloc:
            redirect.remove_header('Authorization')
        return redirect


def read_response(url,token):
    request=urllib.request.Request(url,headers={'Authorization':'Bearer '+token,
        'Accept':'application/vnd.github+json','X-GitHub-Api-Version':'2026-03-10'})
    with urllib.request.build_opener(ArtifactRedirect()).open(request,timeout=60) as response:
        data=response.read(LIMIT+1)
    if len(data)>LIMIT:raise ValueError('Oversized comparison evidence')
    return data


def reference(repository,source,current_run,token):
    if not re.fullmatch(r'[\w.-]+/[\w.-]+',repository) or not re.fullmatch('[0-9a-f]{40}',source):
        raise ValueError('Invalid reference source/repository')
    prefix=f'https://api.github.com/repos/{repository}'
    query=f'/actions/workflows/ci.yml/runs?branch=main&event=push&status=success&head_sha={source}&per_page=100'
    runs=json.loads(read_response(prefix+query,token))['workflow_runs']
    runs=[r for r in runs if r['id']!=int(current_run) and r['head_sha']==source
          and r['repository']['full_name']==repository and r['conclusion']=='success']
    for run in sorted(runs,key=lambda r:r['id'],reverse=True):
        artifacts=json.loads(read_response(prefix+f"/actions/runs/{run['id']}/artifacts?per_page=100",token))
        if artifacts['total_count']!=len(artifacts['artifacts']):
            raise ValueError('Incomplete comparison artifact inventory')
        matches=[a for a in artifacts['artifacts'] if a['name']=='licasa-verification-evidence' and not a['expired']]
        if len(matches)!=1:continue
        content=read_response(prefix+f"/actions/artifacts/{matches[0]['id']}/zip",token)
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            files=[item for item in archive.infolist() if item.filename=='qualification.json']
            if len(files)!=1 or files[0].file_size>LIMIT:raise ValueError('Invalid comparison JSON archive')
            data=json.loads(archive.read(files[0]))
        if data.get('source_sha')!=source or data.get('build_run_id')!=str(run['id']):
            raise ValueError('Comparison artifact identity mismatch')
        return data
    return None


def compare(current,previous):
    if previous is None:
        return {'status':'unavailable','reason':'No retained successful same-source main reference',
                'comparison_passed':False,'differences':[]}
    if (current.get('source_sha')!=previous.get('source_sha')
            or current.get('manifest_sha256')!=previous.get('manifest_sha256')
            or current.get('version')!=previous.get('version')):
        raise ValueError('Comparison source/support/version differ')
    expected=current.get('packages',{})
    if not expected or set(expected)!=set(previous.get('packages',{})):
        raise ValueError('Comparison package inventory differs')
    differences=[name for name in expected if expected[name]!=previous['packages'][name]]
    return {'status':'different' if differences else 'identical',
            'comparison_passed':not differences,'differences':differences,
            'reference_build_run_id':previous['build_run_id'],
            'current_build_run_id':current['build_run_id'],
            'note':'Different bytes require diagnosis of toolchain drift, timestamps or cache dependence; this report is not a hardware/manual release qualification.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--current',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();current=json.loads(args.current.read_text())
    previous=reference(os.environ['GITHUB_REPOSITORY'],current['source_sha'],os.environ['GITHUB_RUN_ID'],os.environ['GH_TOKEN'])
    result=compare(current,previous)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    if result['status']=='different':raise SystemExit('Cold outputs differ from the same-source main reference')
    if result['status']=='unavailable':print('::warning::Reproducibility comparison unavailable; no passing comparison is claimed')


if __name__=='__main__':main()
