#!/usr/bin/env python3

import argparse
import math


DEFAULT_TABLE_SIZE = 2**16 -2
DEFAULT_PROBE_LIMIT = 50
DEFAULT_LIFETIME_INSERTS = 100_000

DEFAULT_ENTRY_COUNTS = [
    100,
    500,
    1_000,
    2_000,
    5_000,
    10_000,
    16_384,
    20_000,
    22_000,
    24_000,
    26_000,
    28_000,
    30_000,
    32_000,
    32_768,
    40_000,
    45_000,
    49_152,
    52_000,
    54_000,
    56_000,
    58_000,
    60_000,
    62_000,
]


def log_choose(n: int, k: int) -> float:
    """Compute log(C(n, k)) without overflow."""
    if k < 0 or k > n:
        return float("-inf")

    return (
        math.lgamma(n + 1)
        - math.lgamma(k + 1)
        - math.lgamma(n - k + 1)
    )


def logsumexp(log_terms: list[float]) -> float:
    """Stable log(sum(exp(log_terms)))."""
    if not log_terms:
        return float("-inf")

    max_log = max(log_terms)

    if max_log == float("-inf"):
        return max_log

    return max_log + math.log(
        sum(math.exp(x - max_log) for x in log_terms)
    )


def logdiffexp(a: float, b: float) -> float:
    """
    Return log(exp(a) - exp(b)), assuming a > b.
    """
    if b >= a:
        raise ValueError("expected a > b")
    return a + math.log1p(-math.exp(b - a))


def log_p_insert_fail_linear_probing(
    table_size: int,
    entry_count: int,
    probe_limit: int,
) -> float:
    """
    Estimate log(P(insert fail)) for a linear-probing hash table.

    Assumptions:
      - table_size slots
      - entry_count occupied slots
      - random uniform hash homes
      - standard linear probing
      - probe_limit is a maximum probe distance, matching FFFS:
        distance 0 through probe_limit are checked
      - insert fails if probe_limit + 1 consecutive probe positions are occupied

    This is cluster-aware. It estimates the probability that the next insert's
    home position lands in a location where it would need more than
    probe_limit distance to find an empty slot.
    """

    m = table_size
    n = entry_count
    probe_window = probe_limit + 1

    if probe_limit < 0:
        raise ValueError("probe_limit must be >= 0")

    if probe_window > m:
        raise ValueError("probe_limit must be <= table_size")

    if n < probe_window:
        return float("-inf")

    if n >= m:
        return 0.0

    log_terms = []

    for cluster_len in range(probe_window, n + 1):
        k = cluster_len

        # Expected number of clusters of exactly length k.
        #
        # For k < n:
        #
        # E[C_k] =
        #   m * C(n, k) * (k + 1)^(k - 1)
        #     * (m - n - 1) * (m - k - 1)^(n - k - 1)
        #     / m^n
        #
        # For k == n:
        #
        # E[C_n] =
        #   m * (n + 1)^(n - 1) / m^n
        #
        # A cluster of length k causes insert failure for:
        #
        #   k - probe_window + 1
        #
        # possible starting positions.

        if k == n:
            log_expected_clusters = (
                math.log(m)
                + (k - 1) * math.log(k + 1)
                - n * math.log(m)
            )
        else:
            if m - n - 1 <= 0 or m - k - 1 <= 0:
                continue

            log_expected_clusters = (
                math.log(m)
                + log_choose(n, k)
                + (k - 1) * math.log(k + 1)
                + math.log(m - n - 1)
                + (n - k - 1) * math.log(m - k - 1)
                - n * math.log(m)
            )

        failing_starts_in_cluster = k - probe_window + 1

        log_probability_contribution = (
            math.log(failing_starts_in_cluster)
            + log_expected_clusters
            - math.log(m)
        )

        log_terms.append(log_probability_contribution)

    return logsumexp(log_terms)


def p_insert_fail_linear_probing(
    table_size: int,
    entry_count: int,
    probe_limit: int,
) -> float:
    log_p = log_p_insert_fail_linear_probing(
        table_size=table_size,
        entry_count=entry_count,
        probe_limit=probe_limit,
    )
    if log_p == float("-inf"):
        return 0.0
    return math.exp(log_p)


def log_p_insert_fail_churn_delete_corrected(
    table_size: int,
    entry_count: int,
    probe_limit: int,
) -> float:
    m = table_size
    n = entry_count
    L = probe_limit + 1

    if n < L:
        return float("-inf")

    if n >= m:
        return 0.0

    log_p_n = log_p_insert_fail_linear_probing(m, n, probe_limit)
    log_p_np1 = log_p_insert_fail_linear_probing(m, n + 1, probe_limit)

    log_delta_p = logdiffexp(log_p_np1, log_p_n)

    return (
        math.log(n + 1 - L)
        - math.log(L)
        + log_delta_p
    )


def p_insert_fail_churn_delete_corrected(
    table_size: int,
    entry_count: int,
    probe_limit: int,
) -> float:
    log_p = log_p_insert_fail_churn_delete_corrected(
        table_size=table_size,
        entry_count=entry_count,
        probe_limit=probe_limit,
    )
    if log_p == float("-inf"):
        return 0.0
    return math.exp(log_p)


def p_early_failure_over_lifetime(
    p_insert_fail: float,
    lifetime_inserts: int,
) -> float:
    """
    Probability of at least one insert failure over lifetime_inserts attempts.

    P(early failure) = 1 - (1 - P(insert fail))^lifetime_inserts

    Uses log1p/expm1 for numerical stability.
    """

    if lifetime_inserts <= 0:
        return 0.0

    if p_insert_fail <= 0.0:
        return 0.0

    if p_insert_fail >= 1.0:
        return 1.0

    return -math.expm1(
        lifetime_inserts * math.log1p(-p_insert_fail)
    )


def format_probability(p: float) -> str:
    if p == 0.0:
        return "0"

    if p < 1e-4:
        return f"{p:.6e}"

    return f"{p:.9f}"


def format_percent(p: float) -> str:
    percent = 100.0 * p

    if percent == 0.0:
        return "0%"

    if percent < 1e-6:
        return f"{percent:.6e}%"

    return f"{percent:.9f}%"


def format_load_percent(p: float) -> str:
    return f"{100.0 * p:.2f}%"


def format_markdown_probability(p: float) -> str:
    if p == 0.0:
        return "0"

    if p < 1e-3:
        return f"{p:.2e}"

    return f"{p:.3f}"


def format_one_in(p: float) -> str:
    if p <= 0.0:
        return "infinite"

    n = 1.0 / p
    
    if n > 1000000000.0:
        return f"{n:.6e}"

    return f"{n:,.0f}"


def format_markdown_one_in(p: float) -> str:
    if p <= 0.0:
        return "infinite"

    n = 1.0 / p
    if n >= 1000000000.0:
        return f"{n:.2e}"

    return f"{n:,.0f}"


def parse_entry_counts(s: str) -> list[int]:
    return [int(x.strip().replace("_", "")) for x in s.split(",") if x.strip()]


def print_markdown_table(rows: list[tuple[int, float, float, float]]) -> None:
    print("|  Files | Slot Load | P(insert fail) | P(early fail) |     1 in N |")
    print("|-------:|----------:|---------------:|--------------:|-----------:|")
    for entry_count, load_factor, p_insert_fail, p_early_failure in rows:
        print(
            f"| {entry_count:6,d} "
            f"| {format_load_percent(load_factor):>9} "
            f"| {format_markdown_probability(p_insert_fail):>14} "
            f"| {format_markdown_probability(p_early_failure):>13} "
            f"| {format_markdown_one_in(p_early_failure):>10} |"
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Calculate linear-probing insert failure probability "
                    "with a bounded probe limit."
    )

    parser.add_argument(
        "--table-size",
        type=int,
        default=DEFAULT_TABLE_SIZE,
        help=f"Hash table size. Default: {DEFAULT_TABLE_SIZE}",
    )

    parser.add_argument(
        "--probe-limit",
        type=int,
        default=DEFAULT_PROBE_LIMIT,
        help=f"Maximum probes before insert failure. Default: {DEFAULT_PROBE_LIMIT}",
    )

    parser.add_argument(
        "--lifetime-inserts",
        type=int,
        default=DEFAULT_LIFETIME_INSERTS,
        help="Insert attempts per file over device lifetime. "
             f"Default: {DEFAULT_LIFETIME_INSERTS}",
    )

    parser.add_argument(
        "--entries",
        type=parse_entry_counts,
        default=DEFAULT_ENTRY_COUNTS,
        help="Comma-separated entry counts, e.g. 10000,12000,14000,32768",
    )

    parser.add_argument(
        "--failure-model",
        choices=["insert-only", "churn-delete"],
        default="insert-only",
        help="Failure model. insert-only uses the classic clustered "
             "linear-probing model; churn-delete applies the delete-hole "
             "load-step correction. Default: insert-only",
    )

    args = parser.parse_args()

    print(f"table_size       = {args.table_size:,}")
    print(f"probe_limit      = {args.probe_limit:,}")
    print(f"lifetime_inserts = {args.lifetime_inserts:,} per file")
    print(f"failure_model    = {args.failure_model}")
    print()

    rows = []
    for entry_count in args.entries:
        load_factor = entry_count / args.table_size

        if args.failure_model == "churn-delete":
            p_insert_fail = p_insert_fail_churn_delete_corrected(
                table_size=args.table_size,
                entry_count=entry_count,
                probe_limit=args.probe_limit,
            )
        else:
            p_insert_fail = p_insert_fail_linear_probing(
                table_size=args.table_size,
                entry_count=entry_count,
                probe_limit=args.probe_limit,
            )

        p_early_failure = p_early_failure_over_lifetime(
            p_insert_fail=p_insert_fail,
            lifetime_inserts=entry_count * args.lifetime_inserts,
        )

        rows.append((entry_count, load_factor, p_insert_fail, p_early_failure))

    print(
        f"{'entries':>10} "
        f"{'load':>10} "
        f"{'P(insert fail)':>18} "
        f"{'P(early failure)':>20} "
        f"{'early fail rate':>18} "
        f"{'approx 1 in N':>16}"
    )

    print("-" * 100)

    for entry_count, load_factor, p_insert_fail, p_early_failure in rows:
        print(
            f"{entry_count:10,d} "
            f"{load_factor:10.6f} "
            f"{format_probability(p_insert_fail):>18} "
            f"{format_probability(p_early_failure):>20} "
            f"{format_percent(p_early_failure):>18} "
            f"{format_one_in(p_early_failure):>16}"
        )

    print()
    print_markdown_table(rows)


if __name__ == "__main__":
    main()
