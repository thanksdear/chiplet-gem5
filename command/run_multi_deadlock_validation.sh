#!/bin/bash

set -uo pipefail

# Usage:
#   bash command/run_multi_deadlock_validation.sh 2
#   bash command/run_multi_deadlock_validation.sh 4
#
# The argument selects how many chiplets independently form the same true
# cross-layer dependency cycle at the same time.

DEADLOCK_RINGS=${1:-2}
export DEADLOCK_RINGS

exec bash command/run_deadlock_validation.sh
