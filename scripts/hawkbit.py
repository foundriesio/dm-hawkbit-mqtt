from __future__ import print_function

import argparse
import pprint
import requests
import json
import sys

__version__ = 1.0

user = 'admin'
password = 'admin'
DS_URL_DEFAULT = 'http://localhost:8080/rest/v1/distributionsets'
SM_URL_DEFAULT = 'http://localhost:8080/rest/v1/softwaremodules'


def publish(provider, name, type, version, description, artifact,
            ds_url, sm_url):
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

    if response.status_code != 500:
        response = json.loads(response.content)

        print('Got response from server when posting software module:')
        pprint.pprint(response)
        artifacts_url = None
        self_url = None
        type_url = None
        metadata_url = None

        if 'errorCode' in response:
            print('An error occurred; stopping.', file=sys.stderr)
            return

        for item in response:
            if 'id' in item:
                id = item['id']
            if '_links' in item:
                if 'artifacts' in item['_links']:
                    artifacts_url = item['_links']['artifacts']['href']
                if 'self' in item['_links']:
                    self_url = item['_links']['self']['href']
                if 'type' in item['_links']:
                    type_url = item['_links']['type']['href']
                if 'metadata' in item['_links']:
                    metadata_url = item['_links']['metadata']['href']

        if None in (artifacts_url, self_url, type_url, metadata_url):
            print("Couldn't parse response", file=sys.stderr)
            return

    # Upload Artifact
    headers = {'Accept': 'application/json'}
    artifacts = {'file': open(artifact, 'rb')}
    response = requests.post(artifacts_url, auth=(user, password),
                             headers=headers, files=artifacts)
    if response.status_code != 500:
        headers = {'Content-Type': 'application/json',
                   'Accept': 'application/json'}
        ds = {'requiredMigrationStep': False,
              'vendor': provider,
              'name': name,
              'type': type,
              'description': description,
              'version': version,
              'modules': [{'id': id}],
              '_links': {'artifacts': artifacts_url,
                         'self': self_url,
                         'type': type_url,
                         'metadata': metadata_url}}
        response = requests.post(ds_url, data=json.dumps([ds]),
                                 auth=(user, password), headers=headers)
        if response.status_code != 500:
            print(response.content)


def main():
    description = 'Simple Hawkbit API Wrapper'
    parser = argparse.ArgumentParser(version=__version__,
                                     description=description)
    parser.add_argument('-p', '--provider', help='SW Module Provider',
                        required=True)
    parser.add_argument('-n', '--name', help='Name', required=True)
    parser.add_argument('-t', '--type', help='Name', required=True)
    parser.add_argument('-sv', '--swversion', help='Version', required=True)
    parser.add_argument('-d', '--description', help='Version', required=True)
    parser.add_argument('-f', '--file', help='Version', required=True)
    parser.add_argument('-ds', '--distribution-sets',
                        help='Distribution Sets URL', default=DS_URL_DEFAULT)
    parser.add_argument('-sm', '--software-modules',
                        help='Software Modules URL', default=SM_URL_DEFAULT)
    args = parser.parse_args()
    publish(args.provider, args.name, args.type, args.swversion,
            args.description, args.file, args.distribution_sets,
            args.software_modules)


if __name__ == '__main__':
    main()
