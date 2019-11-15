#!/bin/bash

# Triggers building nightly release by sending API request
# to CirrusCI where the job is run.
#
# Tools required in the environment that runs this:
#
# - bash
# - curl

set -o errexit

# Verify ENV is set up correctly
# We validate all that need to be set in case, in an absolute emergency,
# we need to run this by hand. Otherwise the GitHub actions environment should
# provide all of these if properly configured
#if [[ -z "${CIRRUS_ACCESS_TOKEN}" ]]; then
#  echo -e "\e[31mA cirrus access token needs to be set in CIRRUS_ACCESS_TOKEN."
#  echo -e "Exiting.\e[0m"
#  exit 1
#fi

# no unset variables allowed from here on out
# allow above so we can display nice error messages for expected unset variables
set -o nounset

curl -X POST https://api.cirrus-ci.com/graphql \
  -H 'authorization: Bearer 3uf30rrv4ef0rtfm18glu4lqsl9n9k2nskmaa8e' \
  -d '{
    "query": "mutation($input: RepositoryCreateBuildInput!) { createBuild(input: $input) { build { id, repositoryId, status }, clientMutationId } }",
    "variables": {
      "input": {
        "repositoryId": 5677320443527168,
        "clientMutationId": "lib/llvm: macOS",
        "branch": "nightly-to-cirrus"
      }
    }
  }'
