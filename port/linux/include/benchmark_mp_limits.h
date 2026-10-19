/* Shared capacity for private synthetic multiplayer benchmark players. */
#ifndef HALO_BENCHMARK_MP_LIMITS_H
#define HALO_BENCHMARK_MP_LIMITS_H

#include "halo_port_limits.h"

#define BENCHMARK_MP_MAXIMUM_PLAYERS 128
#if BENCHMARK_MP_MAXIMUM_PLAYERS != HALO_PORT_MAXIMUM_NETWORK_PLAYERS
#error "Benchmark actor capacity must match the native network roster"
#endif

#endif
