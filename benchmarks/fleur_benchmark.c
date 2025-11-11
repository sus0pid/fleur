#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fleur.h>

enum {
    kTargetEntries = 15,
    kMaxKeyBytes = 32
};

static const double kTargetFalsePositive = 0.000001; /* 0.0001% */
static const size_t kLookupBatches = 1u << 18;
static const size_t kRandomTrials = 1u << 20;

static inline double elapsed_ns(const struct timespec start,
                                const struct timespec end) {
    const double sec = (double)(end.tv_sec - start.tv_sec) * 1e9;
    const double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec + nsec;
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static BloomFilter create_target_filter(void) {
    BloomFilter bf = fleur_initialize(kTargetEntries, kTargetFalsePositive, "");
    if (bf.v == NULL) {
        bf.error = 1;
    }
    return bf;
}

int main(void) {
    BloomFilter bf = create_target_filter();
    if (bf.error != 0 || bf.v == NULL) {
        fprintf(stderr, "Failed to allocate bloom filter buffers\n");
        return EXIT_FAILURE;
    }

    char inserted[kTargetEntries][kMaxKeyBytes];
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint64_t i = 0; i < kTargetEntries; ++i) {
        snprintf(inserted[i], kMaxKeyBytes, "peer-%02" PRIu64, i);
        int rc = fleur_add(&bf, inserted[i], strlen(inserted[i]));
        if (rc == -1) {
            fprintf(stderr, "Filter saturated while inserting peer-%02" PRIu64 "\n", i);
            fleur_destroy_filter(&bf);
            return EXIT_FAILURE;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    const double insert_ns_per_op = elapsed_ns(start, end) / kTargetEntries;

    for (uint64_t i = 0; i < kTargetEntries; ++i) {
        if (fleur_check(&bf, inserted[i], strlen(inserted[i])) != 1) {
            fprintf(stderr, "Filter lookup failed for %s\n", inserted[i]);
            fleur_destroy_filter(&bf);
            return EXIT_FAILURE;
        }
    }

    const size_t total_member_checks = kLookupBatches * kTargetEntries;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t batch = 0; batch < kLookupBatches; ++batch) {
        for (uint64_t i = 0; i < kTargetEntries; ++i) {
            (void)fleur_check(&bf, inserted[i], strlen(inserted[i]));
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    const double member_lookup_ns = elapsed_ns(start, end) / total_member_checks;

    size_t false_hits = 0;
    uint64_t rng_state = 0x4d595df4d0f33173ULL;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t i = 0; i < kRandomTrials; ++i) {
        uint64_t candidate = xorshift64(&rng_state);
        char key[kMaxKeyBytes];
        snprintf(key, sizeof(key), "noise-%016" PRIx64, candidate);
        if (fleur_check(&bf, key, strlen(key)) == 1) {
            ++false_hits;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    const double random_lookup_ns = elapsed_ns(start, end) / (double)kRandomTrials;
    const double measured_fp = (double)false_hits / (double)kRandomTrials;

    const unsigned wire_bits = (unsigned)bf.h.m;
    const size_t wire_bytes = bf.M * sizeof(uint64_t);

    printf("fleur benchmark\n");
    printf(" peers: %" PRIu64 "\n bitmap: %zu bytes (%u bits)\n hashes: %" PRIu64 "\n target FPR: %.5f%%\n",
           bf.h.n,
           wire_bytes,
           wire_bits,
           bf.h.k,
           kTargetFalsePositive * 100.0);
    printf(" inserts: %.2f ns/op for %" PRIu64 " peers\n", insert_ns_per_op, bf.h.n);
    printf(" member lookups: %.2f ns/op over %zu checks\n",
           member_lookup_ns, total_member_checks);
    printf(" random lookups: %.2f ns/op over %zu checks\n",
           random_lookup_ns, (size_t)kRandomTrials);
    printf(" observed FPR: %.6f%% (%zu / %zu)\n",
           measured_fp * 100.0,
           false_hits,
           (size_t)kRandomTrials);

    fleur_destroy_filter(&bf);
    return EXIT_SUCCESS;
}
