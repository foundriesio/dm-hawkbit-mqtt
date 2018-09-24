from __future__ import print_function

import argparse
import pprint
import requests
import json
import sys
import time

user = 'admin'
password = 'admin'
DS_URL_DEFAULT = 'http://localhost:8080/rest/v1/distributionsets'
SM_URL_DEFAULT = 'http://localhost:8080/rest/v1/softwaremodules'
RO_URL_DEFAULT = 'http://localhost:8080/rest/v1/rollouts'

ROLLOUT_START_DELAY = 5

def start_rollout(ro_id, start_url):
    sys.stdout.write('Starting rollout in ' + str(ROLLOUT_START_DELAY) +
                     ' seconds ... ')
    sys.stdout.flush()
    time.sleep(ROLLOUT_START_DELAY)
    response = requests.post(start_url, auth=(user, password))

    if response.status_code != 200:
        print('Error!')
        return -response.status_code

    print('Started.')
    return ro_id

def create_rollout(version, ds_id, ro_url, rollout_filter, counter, verbose):
    headers = {'Content-Type': 'application/hal+json;charset=UTF-8',
               'Accept': 'application/hal+json'}
    rollout_name = 'RO-' + version + '-' + str(counter)
    ds = {'distributionSetId': ds_id,
          'successCondition': {'condition': 'THRESHOLD', 'expression': 100},
          'successAction': {'expression': '', 'action': 'NEXTGROUP'},
          'targetFilterQuery': 'name==' + rollout_filter + '*',
          'name': rollout_name,
          'description': 'Rollout for ' + version + ' to ' + rollout_filter,
          'amountGroups': 1,
          'errorAction': {'expression': '', 'action': 'PAUSE'},
          'errorCondition': {'condition': 'THRESHOLD', 'expression': 80}
    }
    print('Creating Rollout: ' + rollout_name)
    response = requests.post(ro_url, data=json.dumps(ds),
                             auth=(user, password), headers=headers)

    if response.status_code != 201:
        return -response.status_code

    response = response.json()

    if verbose:
        print('Got response from server when creating rollout:')
        pprint.pprint(response)

    ro_id = int(response.get('id', -1))
    start_url = response['_links']['start'].get('href')

    if start_url is None or ro_id == -1:
        print("Couldn't parse response", file=sys.stderr)
        return -1

    return start_rollout(ro_id, start_url)

def publish_ds(provider, name, type, version, description, artifact,
               ds_url, ro_url, rollout_filter, sm_id, artifacts_url, self_url,
               type_url, metadata_url, counter, verbose):
    # Upload Artifact
    headers = {'Accept': 'application/json'}
    with open(artifact, 'rb') as f:
        artifacts = {'file': f}
        print('Uploading artifact: ' + artifact)
        response = requests.post(artifacts_url, auth=(user, password),
                                 headers=headers, files=artifacts)
        if response.status_code != 201:
            return -response.status_code

    headers = {'Content-Type': 'application/json',
               'Accept': 'application/json'}
    ds_version = version + '-' + str(counter)
    ds = {'requiredMigrationStep': False,
          'vendor': provider,
          'name': name,
          'type': type,
          'description': description,
          'version': ds_version,
          'modules': [{'id': sm_id}],
          '_links': {'artifacts': artifacts_url,
                     'self': self_url,
                     'type': type_url,
                     'metadata': metadata_url}}
    print('Creating Distribution Set: ' + name + ' [' + ds_version + ']')
    response = requests.post(ds_url, data=json.dumps([ds]),
                             auth=(user, password), headers=headers)

    if response.status_code != 201:
        return -response.status_code

    response = response.json()

    if verbose:
        print('Got response from server when posting artifacts:')
        pprint.pprint(response)

    if rollout_filter is None:
        return 0

    ds_id = 0

    for item in response:
        ds_id = int(item.get('id', 0))

    if ds_id == 0:
        print("Couldn't parse artifact post response", file=sys.stderr)
        return -1

    return create_rollout(version, ds_id, ro_url, rollout_filter, counter,
                          verbose)

def read_sm(provider, name, type, version, description, artifact,
            ds_url, ro_url, rollout_filter, id, self_url, counter, verbose):
    # Read back detailed softwaremodule info
    headers = {'Accept': 'application/json'}
    print('Getting URLs from Software Module: ' + name + ' [' + version +
          '-' + str(counter) + ']')
    response = requests.get(self_url,
                            auth=(user, password), headers=headers)

    if response.status_code != 200:
         return -response.status_code

    response = response.json()

    if verbose:
        print('Got response from server when reading new software module:')
        pprint.pprint(response)

    if 'errorCode' in response:
        print('An error occurred; stopping.', file=sys.stderr)
        return -1

    artifacts_url = response['_links']['artifacts'].get('href')
    type_url = response['_links']['type'].get('href')
    metadata_url = response['_links']['metadata'].get('href')

    if None in (artifacts_url, type_url, metadata_url):
        print("Couldn't parse response", file=sys.stderr)
        return -1

    return publish_ds(provider, name, type, version, description, artifact,
                      ds_url, ro_url, rollout_filter, id, artifacts_url,
                      self_url, type_url, metadata_url, counter, verbose)


def publish_sm(provider, name, type, version, description, artifact,
               ds_url, sm_url, ro_url, rollout_filter, counter, verbose):
    # Publish Software Module
    headers = {'Content-Type': 'application/json',
               'Accept': 'application/json'}
    sm_version = version + '-' + str(counter)
    sm = {'requiredMigrationStep': False,
          'vendor': provider,
          'name': name,
          'type': type,
          'description': description,
          'version': sm_version}
    print('Creating Software Module: ' + name + ' [' + sm_version + ']')
    response = requests.post(sm_url, data=json.dumps([sm]),
                             auth=(user, password), headers=headers)

    if response.status_code != 201:
        return -response.status_code

    response = response.json()

    if verbose:
        print('Got response from server when posting software module:')
        pprint.pprint(response)

    if 'errorCode' in response:
        print('An error occurred; stopping.', file=sys.stderr)
        return -1

    id = -1
    self_url = None

    for item in response:
        id = int(item.get('id', -1))
        self_url = item['_links']['self'].get('href')

    if self_url is None or id == -1:
        print("Couldn't parse response", file=sys.stderr)
        return -1

    return read_sm(provider, name, type, version, description, artifact,
                   ds_url, ro_url, rollout_filter, id, self_url, counter,
                   verbose)


def is_rollout_finished(ro_id, ro_url, verbose):
    # Read current rollouts
    headers = {'Accept': 'application/json'}
    response = requests.get(ro_url + '/' + str(ro_id),
                            auth=(user, password), headers=headers)

    if response.status_code != 200:
         return -response.status_code

    response = response.json()

    if verbose:
        print('Got response from server when checking rollout status:')
        pprint.pprint(response)

    targets = int(response.get('totalTargets', -1))
    cancelled = int(response['totalTargetsPerStatus'].get('cancelled', 0))
    finished = int(response['totalTargetsPerStatus'].get('finished', 0))
    errored = int(response['totalTargetsPerStatus'].get('error', 0))

    if cancelled + finished + errored == targets:
        return 1
    else:
        return 0

def count_rollouts(name, version, ro_url):
    # Read current rollouts
    headers = {'Accept': 'application/json'}
    response = requests.get(ro_url,
                            auth=(user, password), headers=headers)

    if response.status_code != 200:
         return -response.status_code

    response = response.json()

    find_count = 0

    for item in response['content']:
        rollout_name = str(item.get('name', ''))
        if rollout_name.find('RO-' + version) > -1:
            find_count += 1

    return find_count

def main():
    description = 'Simple Hawkbit API Wrapper'
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('-p', '--provider', help='SW Module provider',
                        default='Foundries.io')
    parser.add_argument('-n', '--name', default='dm-hawkbit-mqtt',
                        help='SW Module name, default: dm-hawkbit-mqtt')
    parser.add_argument('-t', '--type', default='os',
                        help='SW Module type, default: os')
    parser.add_argument('-sv', '--swversion', help='SW Module version',
                        required=True)
    parser.add_argument('-d', '--description',
                        default='Foundries.io dm-hawkbit-mqtt reference app',
                        help='SW Module description')
    parser.add_argument('-f', '--file', help='Artifact to upload', required=True)
    parser.add_argument('-ds', '--distribution-sets',
                        help='Distribution Sets URL', default=DS_URL_DEFAULT)
    parser.add_argument('-sm', '--software-modules',
                        help='Software Modules URL', default=SM_URL_DEFAULT)
    parser.add_argument('-ro', '--rollouts',
                        help='Rollouts URL', default=RO_URL_DEFAULT)
    parser.add_argument('-rf', '--rollout-filter', help='Rollout name filter',
                        default=None)
    parser.add_argument('-rc', '--rollout-count', help='Rollout count',
                        type=int, default=1)
    parser.add_argument('-vv', '--verbose', help='Verbose output',
                        default=False)
    args = parser.parse_args()

    args.verbose = bool(args.verbose)
    existing_rollouts = count_rollouts(args.name, args.swversion,
                                       args.rollouts)
    if existing_rollouts < 0:
        print("Count rollouts error: " + str(existing_rollouts))
        return

    print("Found existing rollouts: " + str(existing_rollouts))
    ro_id = 1

    while existing_rollouts < args.rollout_count and ro_id > 0:
        ro_id = publish_sm(args.provider, args.name, args.type, args.swversion,
                           args.description, args.file, args.distribution_sets,
                           args.software_modules, args.rollouts,
                           args.rollout_filter, existing_rollouts, args.verbose)

        if args.rollout_count > 1 and ro_id > 0:
            rollout_finished = 0
            print_counter = 0
            print('Waiting for rollout: RO-' + args.swversion +
                  '-' + str(existing_rollouts) + ' to finish.')
            while rollout_finished == 0:
                time.sleep(5)
                print_counter += 1
                sys.stdout.write('.')
                sys.stdout.flush()
                if print_counter == 40:
                    print('')
                    print_counter = 0
                rollout_finished = is_rollout_finished(ro_id, args.rollouts,
                                                       args.verbose)
                if rollout_finished < 0:
                    ro_id = rollout_finished

            print('')
            print('Finished.')

        existing_rollouts += 1

    if ro_id < 0:
        print('Script aborted due to error: ' + str(ro_id))


if __name__ == '__main__':
    main()
