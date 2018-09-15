from __future__ import print_function

import argparse
import pprint
import requests
import json
import sys
import time

__version__ = 1.0

user = 'admin'
password = 'admin'
DS_URL_DEFAULT = 'http://localhost:8080/rest/v1/distributionsets'
SM_URL_DEFAULT = 'http://localhost:8080/rest/v1/softwaremodules'
RO_URL_DEFAULT = 'http://localhost:8080/rest/v1/rollouts'

ROLLOUT_START_DELAY = 5

def start_rollout(version, ds_id, ro_url, rollout_filter):
    headers = {'Content-Type': 'application/hal+json;charset=UTF-8',
               'Accept': 'application/hal+json'}
    ds = {'distributionSetId': ds_id,
          'successCondition': {'condition': 'THRESHOLD', 'expression': 100},
          'successAction': {'expression': '', 'action': 'NEXTGROUP'},
          'targetFilterQuery': 'name==' + rollout_filter + '*',
          'name': 'RO-' + version,
          'description': 'Rollout for ' + version + ' to ' + rollout_filter,
          'amountGroups': 1,
          'errorAction': {'expression': '', 'action': 'PAUSE'},
          'errorCondition': {'condition': 'THRESHOLD', 'expression': 80}
    }
    response = requests.post(ro_url, data=json.dumps(ds),
                             auth=(user, password), headers=headers)

    if response.status_code == 500:
        return

    response = response.json()

    print('Got response from server when creating rollout:')
    pprint.pprint(response)

    start_url = response['_links']['start'].get('href')

    if start_url is None:
        print("Couldn't parse response", file=sys.stderr)
        return

    print('Starting rollout ...')
    time.sleep(ROLLOUT_START_DELAY)
    response = requests.post(start_url, auth=(user, password))

    if response.status_code == 500:
        return

    print('Started.')


def publish_ds(provider, name, type, version, description, artifact,
               ds_url, ro_url, rollout_filter, sm_id, artifacts_url, self_url,
               type_url, metadata_url):
    # Upload Artifact
    headers = {'Accept': 'application/json'}
    with open(artifact, 'rb') as f:
        artifacts = {'file': f}
        response = requests.post(artifacts_url, auth=(user, password),
                                 headers=headers, files=artifacts)
        if response.status_code == 500:
            return

    headers = {'Content-Type': 'application/json',
               'Accept': 'application/json'}
    ds = {'requiredMigrationStep': False,
          'vendor': provider,
          'name': name,
          'type': type,
          'description': description,
          'version': version,
          'modules': [{'id': sm_id}],
          '_links': {'artifacts': artifacts_url,
                     'self': self_url,
                     'type': type_url,
                     'metadata': metadata_url}}
    response = requests.post(ds_url, data=json.dumps([ds]),
                             auth=(user, password), headers=headers)

    if response.status_code == 500:
        return

    response = response.json()

    print('Got response from server when posting artifacts:')
    pprint.pprint(response)

    if rollout_filter is None:
        return

    ds_id = 0

    for item in response:
        ds_id = int(item.get('id', 0))

    if ds_id == 0:
        print("Couldn't parse artifact post response", file=sys.stderr)
        return

    start_rollout(version, ds_id, ro_url, rollout_filter)

def read_sm(provider, name, type, version, description, artifact,
            ds_url, ro_url, rollout_filter, id, self_url):
    # Read back detailed softwaremodule info
    headers = {'Accept': 'application/json'}
    response = requests.get(self_url,
                            auth=(user, password), headers=headers)

    if response.status_code == 500:
         return

    response = response.json()

    print('Got response from server when reading new software module:')
    pprint.pprint(response)

    if 'errorCode' in response:
        print('An error occurred; stopping.', file=sys.stderr)
        return

    artifacts_url = response['_links']['artifacts'].get('href')
    type_url = response['_links']['type'].get('href')
    metadata_url = response['_links']['metadata'].get('href')

    if None in (artifacts_url, type_url, metadata_url):
        print("Couldn't parse response", file=sys.stderr)
        return

    publish_ds(provider, name, type, version, description, artifact,
               ds_url, ro_url, rollout_filter, id, artifacts_url, self_url,
               type_url, metadata_url)


def publish_sm(provider, name, type, version, description, artifact,
               ds_url, sm_url, ro_url, rollout_filter):
    # Publish Software Module
    headers = {'Content-Type': 'application/json',
               'Accept': 'application/json'}
    sm = {'requiredMigrationStep': False,
          'vendor': provider,
          'name': name,
          'type': type,
          'description': description,
          'version': version}
    response = requests.post(sm_url, data=json.dumps([sm]),
                             auth=(user, password), headers=headers)

    if response.status_code == 500:
        return

    response = response.json()

    print('Got response from server when posting software module:')
    pprint.pprint(response)

    if 'errorCode' in response:
        print('An error occurred; stopping.', file=sys.stderr)
        return

    id = -1
    self_url = None

    for item in response:
        id = int(item.get('id', -1))
        self_url = item['_links']['self'].get('href')

    if self_url is None or id == -1:
        print("Couldn't parse response", file=sys.stderr)
        return

    read_sm(provider, name, type, version, description, artifact,
            ds_url, ro_url, rollout_filter, id, self_url)


def main():
    description = 'Simple Hawkbit API Wrapper'
    parser = argparse.ArgumentParser(version=__version__,
                                     description=description)
    parser.add_argument('-p', '--provider', help='SW Module provider',
                        required=True)
    parser.add_argument('-n', '--name', help='SW Module name',
                        required=True)
    parser.add_argument('-t', '--type', help='SW Module type', required=True)
    parser.add_argument('-sv', '--swversion', help='SW Module version',
                        required=True)
    parser.add_argument('-d', '--description', help='SW Module description',
                        required=True)
    parser.add_argument('-f', '--file', help='Artifact to upload', required=True)
    parser.add_argument('-ds', '--distribution-sets',
                        help='Distribution Sets URL', default=DS_URL_DEFAULT)
    parser.add_argument('-sm', '--software-modules',
                        help='Software Modules URL', default=SM_URL_DEFAULT)
    parser.add_argument('-ro', '--rollouts',
                        help='Rollouts URL', default=RO_URL_DEFAULT)
    parser.add_argument('-rf', '--rollout-filter', help='Rollout name filter',
                        default=None)
    args = parser.parse_args()
    publish_sm(args.provider, args.name, args.type, args.swversion,
               args.description, args.file, args.distribution_sets,
               args.software_modules, args.rollouts, args.rollout_filter)


if __name__ == '__main__':
    main()
