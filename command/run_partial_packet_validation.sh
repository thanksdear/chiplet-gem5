#!/bin/bash

set -uo pipefail

# The victim packet head participates in the real dependency cycle while its
# body and tail remain upstream.  Recovery must wait for all five ordered
# flits after returning credits for the absorbed head.

export DEADLOCK_RINGS=1
export DEADLOCK_PARTIAL_PACKET=1
export PARTIAL_TAIL_DELAY_CYCLES=${PARTIAL_TAIL_DELAY_CYCLES:-520}

exec bash command/run_deadlock_validation.sh
