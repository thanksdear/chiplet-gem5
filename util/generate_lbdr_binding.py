#!/usr/bin/env python3
"""Generate a static LBDR-lite gateway binding for one 4x4 chiplet.

This is a deliberately small reproduction of the offline node-binding stage
from Cao et al., "LBDR: A load-balanced deadlock-free routing strategy for
chiplet systems" (Integration, 2024).  It uses simulated annealing to balance
the offered source load across the four fixed corner gateways in the current
Chiplet2_5D topology.
"""

import argparse
import math
import random
import statistics


ROWS = 4
COLS = 4
NUM_ROUTERS = ROWS * COLS
GATEWAYS = ((0, 0), (3, 0), (0, 3), (3, 3))


def parse_weights(text):
    values = [float(value.strip()) for value in text.split(",")
              if value.strip()]
    if len(values) != NUM_ROUTERS:
        raise ValueError("exactly 16 comma-separated weights are required")
    if any(value < 0 for value in values) or sum(values) <= 0:
        raise ValueError("weights must be non-negative and not all zero")
    return values


def distance(local_router, gateway):
    x = local_router % COLS
    y = local_router // COLS
    gateway_x, gateway_y = GATEWAYS[gateway]
    return abs(x - gateway_x) + abs(y - gateway_y)


def loads(binding, weights):
    result = [0.0] * len(GATEWAYS)
    for local_router, gateway in enumerate(binding):
        result[gateway] += weights[local_router]
    return result


def cost(binding, weights, distance_weight):
    gateway_loads = loads(binding, weights)
    mean_load = sum(gateway_loads) / len(gateway_loads)
    imbalance = statistics.pstdev(gateway_loads) / mean_load
    weighted_hops = sum(
        weights[router] * distance(router, gateway)
        for router, gateway in enumerate(binding)
    ) / sum(weights)
    normalized_hops = weighted_hops / (ROWS + COLS - 2)
    return imbalance + distance_weight * normalized_hops


def anneal(weights, iterations, seed, distance_weight):
    rng = random.Random(seed)
    current = [
        min(range(len(GATEWAYS)), key=lambda gateway:
            (distance(router, gateway), gateway))
        for router in range(NUM_ROUTERS)
    ]
    current_cost = cost(current, weights, distance_weight)
    best = current[:]
    best_cost = current_cost

    for step in range(iterations):
        fraction = step / max(iterations - 1, 1)
        temperature = 0.25 * math.pow(1e-4, fraction)
        router = rng.randrange(NUM_ROUTERS)
        old_gateway = current[router]
        new_gateway = rng.randrange(len(GATEWAYS) - 1)
        if new_gateway >= old_gateway:
            new_gateway += 1

        current[router] = new_gateway
        candidate_cost = cost(current, weights, distance_weight)
        delta = candidate_cost - current_cost
        if delta <= 0 or rng.random() < math.exp(-delta / temperature):
            current_cost = candidate_cost
            if candidate_cost < best_cost:
                best = current[:]
                best_cost = candidate_cost
        else:
            current[router] = old_gateway

    return best, best_cost


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--weights",
        default=",".join(["1"] * NUM_ROUTERS),
        help="injection weights for local routers 0-15, comma separated",
    )
    parser.add_argument("--iterations", type=int, default=50000)
    parser.add_argument("--seed", type=int, default=2024)
    parser.add_argument(
        "--distance-weight", type=float, default=0.05,
        help="small path-length tie breaker; load balance remains dominant",
    )
    args = parser.parse_args()

    if args.iterations <= 0:
        parser.error("--iterations must be positive")
    try:
        weights = parse_weights(args.weights)
    except ValueError as error:
        parser.error(str(error))

    binding, result_cost = anneal(
        weights, args.iterations, args.seed, args.distance_weight)
    gateway_loads = loads(binding, weights)

    print("--lbdr-gateway-map=" + ",".join(map(str, binding)))
    print("gateway loads:", ", ".join(f"G{i}={value:g}"
                                      for i, value in
                                      enumerate(gateway_loads)))
    print(f"objective: {result_cost:.6f}")


if __name__ == "__main__":
    main()
