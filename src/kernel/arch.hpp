#pragma once

/* This tree currently targets a single architecture; fail fast on misconfigured toolchains. */
#if !defined(__x86_64__)
#error "This kernel is built for x86_64"
#endif
