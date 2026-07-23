#!/usr/bin/env python3
"""
Analyze bitwidth-plugin CSV output (class,bitwidth,count) from one or more
benchmark runs, and produce summary tables + charts answering the project's
core question: how many effective bits do ALU operands / stored values
actually need at runtime?

Usage:
    analysis/.venv/bin/python analysis/analyze.py [args.result] [output_dir]

Defaults: args.result=benchmarks/results, output_dir=analysis/output
"""
import sys
import glob
import os
import csv
import collections
import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# Palette (dataviz skill reference palette, light mode)
BLUE = "#2a78d6"
BLUE_SOFT = "#9ec5f4"  # sequential step 200, for the density bars (recessive vs the line)
RED = "#e34948"        # categorical slot 6, for the cumulative line (contrasts with blue bars)
GRID = "#d8d7d2"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"

MAX_BITS = 32

parser = argparse.ArgumentParser(description="""
Analyze bitwidth-plugin CSV output (class,bitwidth,count) from one or more \
benchmark runs, and produce summary tables + charts answering the project's \
core question: how many effective bits do ALU operands / stored values \
actually need at runtime?

Usage:
    analysis/.venv/bin/python analysis/analyze.py [args.result] [output_dir]

Defaults: args.result=benchmarks/results, output_dir=analysis/output
"""
)

parser.add_argument("-i", "--result", default="benchmarks/results")
parser.add_argument("-o", "--output", default="analysis/output")
parser.add_argument("-e", "--exclude", default=[])
parser.add_argument("-n", "--no_by_benchmark", action="store_true")
parser.add_argument("-c", "--exclude_crypto", action="store_true")

args = parser.parse_args()

def load_results(result, exclude=()):
    """Returns:
      per_bench: {bench_name: {class: {bitwidth: count}}}
      combined: {class: {bitwidth: count}}

    `args.exclude` is a set of benchmark base names (without .csv) to skip --
    e.g. full crypto implementations that CLAUDE.md's methodology args.excludes
    from the target domain (dedicated accelerator territory).
    """
    per_bench = {}
    combined = collections.defaultdict(lambda: collections.defaultdict(int))
    for path in sorted(glob.glob(os.path.join(result, "*.csv"))):
        name = os.path.splitext(os.path.basename(path))[0]
        if name in exclude:
            continue
        table = collections.defaultdict(lambda: collections.defaultdict(int))
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                cls = row["class"]
                bw = int(row["bitwidth"])
                cnt = int(row["count"])
                table[cls][bw] += cnt
                combined[cls][bw] += cnt
        per_bench[name] = table
    return per_bench, combined


def class_totals(table):
    return {cls: sum(bw_counts.values()) for cls, bw_counts in table.items()}


def percentile_bits(bw_counts, pct):
    """Smallest bitwidth W such that pct% of samples have bitwidth <= W."""
    total = sum(bw_counts.values())
    if total == 0:
        return None
    target = total * pct / 100.0
    acc = 0
    for w in range(0, MAX_BITS + 1):
        acc += bw_counts.get(w, 0)
        if acc >= target:
            return w
    return MAX_BITS


def style_axes(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(GRID)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=9)
    ax.yaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)


def bitwidth_series(bw_counts):
    """xs, per-bin density (%), cumulative (%) -- both on a shared 0-100 scale."""
    total = sum(bw_counts.values())
    xs = list(range(0, MAX_BITS + 1))
    density = [100.0 * bw_counts.get(w, 0) / total if total else 0.0 for w in xs]
    cum, acc = [], 0.0
    for d in density:
        acc += d
        cum.append(acc)
    return xs, density, cum, total


def plot_cumulative_overall(combined, out_path):
    agg = collections.Counter()
    for cls, bw_counts in combined.items():
        for w, c in bw_counts.items():
            agg[w] += c

    xs, density, cum, total = bitwidth_series(agg)

    fig, ax = plt.subplots(figsize=(8, 5), dpi=150)
    ax.bar(xs, density, color=BLUE_SOFT, width=1.0, zorder=2, label="% of operations in this bin")
    ax.plot(xs, cum, color=RED, linewidth=2, zorder=3, label="cumulative %")

    for pct in (50, 90, 99):
        w = next(x for x, y in zip(xs, cum) if y >= pct)
        ax.annotate(f"{pct}% ≤ {w} bits", xy=(w, pct),
                     xytext=(w + 1.2, pct - 8 if pct > 15 else pct + 6),
                     fontsize=8.5, color=TEXT_SECONDARY,
                     arrowprops=dict(arrowstyle="-", color=GRID, lw=0.8))

    style_axes(ax)
    ax.set_xlim(0, MAX_BITS)
    ax.set_ylim(0, 100)
    ax.set_xlabel("effective bit width", fontsize=10, color=TEXT_SECONDARY)
    ax.set_ylabel("% of sampled operations", fontsize=10, color=TEXT_SECONDARY)
    ax.legend(loc="center right", frameon=False, fontsize=8.5, labelcolor=TEXT_SECONDARY)
    ax.set_title(f"How many bits do ALU results / stored values actually need?\n"
                 f"(all classes combined, n={total:,})",
                 fontsize=11, color=TEXT_PRIMARY, loc="left")
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_class_totals(combined, out_path):
    totals = class_totals(combined)
    items = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)
    labels = [k for k, _ in items]
    values = [v for _, v in items]

    fig, ax = plt.subplots(figsize=(8, 6), dpi=150)
    y = range(len(labels))
    ax.barh(y, values, color=BLUE, height=0.65, zorder=2)
    ax.set_yticks(list(y))
    ax.set_yticklabels(labels, fontsize=9)
    ax.invert_yaxis()
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{x/1e6:.1f}M" if x >= 1e6 else f"{int(x):,}"))
    style_axes(ax)
    ax.xaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.yaxis.grid(False)
    ax.set_xlabel("sampled operations", fontsize=10, color=TEXT_SECONDARY)
    ax.set_title("Sampled operation count by instruction class (all benchmarks)",
                 fontsize=11, color=TEXT_PRIMARY, loc="left")
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_benchmark_totals(per_bench, out_path):
    totals = {name: sum(sum(bw.values()) for bw in table.values())
              for name, table in per_bench.items()}
    items = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)
    labels = [k for k, _ in items]
    values = [v for _, v in items]

    fig, ax = plt.subplots(figsize=(8, 5), dpi=150)
    y = range(len(labels))
    ax.barh(y, values, color=BLUE, height=0.6, zorder=2)
    ax.set_yticks(list(y))
    ax.set_yticklabels(labels, fontsize=9)
    ax.invert_yaxis()
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{x/1e6:.2f}M"))
    style_axes(ax)
    ax.xaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.yaxis.grid(False)
    ax.set_xlabel("sampled operations", fontsize=10, color=TEXT_SECONDARY)
    ax.set_title("Sampled operation count by benchmark", fontsize=11, color=TEXT_PRIMARY, loc="left")
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_class_histograms(table, out_path, title, subtitle=None):
    """Small-multiples grid: one subplot per instruction class, each showing
    that class's bit-width density (bars) and cumulative distribution (line)
    on a shared 0-100% axis."""
    totals = class_totals(table)
    classes = [c for c, _ in sorted(totals.items(), key=lambda kv: kv[1], reverse=True) if totals[c] > 0]
    if not classes:
        return False

    ncols = 4
    nrows = -(-len(classes) // ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(ncols * 3.0, nrows * 2.1), dpi=150)
    axes = axes.flatten() if len(classes) > 1 else [axes]

    for i, cls in enumerate(classes):
        ax = axes[i]
        xs, density, cum, total = bitwidth_series(table[cls])
        ax.bar(xs, density, color=BLUE_SOFT, width=1.0, zorder=2)
        ax.plot(xs, cum, color=RED, linewidth=1.3, zorder=3)
        ax.set_title(f"{cls}  (n={total:,})", fontsize=8.5, color=TEXT_PRIMARY, loc="left")
        ax.set_xlim(-0.5, MAX_BITS + 0.5)
        ax.set_ylim(0, 100)
        ax.tick_params(labelsize=6.5, colors=TEXT_SECONDARY)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.spines["left"].set_visible(False)
        ax.spines["bottom"].set_color(GRID)
        ax.set_yticks([])

    for j in range(len(classes), len(axes)):
        axes[j].axis("off")

    handles = [
        plt.Rectangle((0, 0), 1, 1, color=BLUE_SOFT, label="% of this class's operations in this bin"),
        plt.Line2D([0], [0], color=RED, linewidth=1.5, label="cumulative %"),
    ]
    fig.legend(handles=handles, loc="upper right", frameon=False, fontsize=8,
               labelcolor=TEXT_SECONDARY, bbox_to_anchor=(0.99, 0.995))

    suptitle = title
    if subtitle:
        suptitle += f"\n{subtitle}"
    fig.suptitle(suptitle, fontsize=11, color=TEXT_PRIMARY, x=0.01, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.97 if not subtitle else 0.95))
    fig.savefig(out_path)
    plt.close(fig)
    return True


def aggregate_by_benchmark(per_bench):
    """{bench_name: {class: {bitwidth: count}}} -> {bench_name: {bitwidth: count}},
    collapsing all instruction classes into one distribution per benchmark."""
    result = {}
    for name, table in per_bench.items():
        agg = collections.Counter()
        for bw_counts in table.values():
            for w, c in bw_counts.items():
                agg[w] += c
        result[name] = agg
    return result


def print_summary_table(combined):
    totals = class_totals(combined)
    print(f"{'class':6s} {'count':>12s} {'p50':>5s} {'p90':>5s} {'p99':>5s} {'max':>5s}")
    for cls, total in sorted(totals.items(), key=lambda kv: kv[1], reverse=True):
        if total == 0:
            continue
        bw_counts = combined[cls]
        p50 = percentile_bits(bw_counts, 50)
        p90 = percentile_bits(bw_counts, 90)
        p99 = percentile_bits(bw_counts, 99)
        maxw = max(w for w, c in bw_counts.items() if c > 0)
        print(f"{cls:6s} {total:>12,} {p50:>5} {p90:>5} {p99:>5} {maxw:>5}")


#  CRC32 is deliberately kept -- "lightweight checksums" are explicitly
#  in-scope even though, as it turns out, CRC32's XOR operations still
#  exercise near-full-width values.
#
#  picojpeg is excluded alongside the crypto benches: it's a full JPEG
#  decoder including a Winograd IDCT (an FFT-family transform), which
#  CLAUDE.md's methodology treats as dedicated-accelerator territory just
#  like AES/SHA, not softcore-domain code.
CRYPTO_EXCLUDE = {"nettle-aes", "nettle-sha256", "md5sum", "aha-mont64", "picojpeg"}


def main():
    # args.exclude = set(args.exclude_arg.split(",")) if args.exclude_arg else set()
    os.makedirs(args.output, exist_ok=True)
    exclude = []
    if args.exclude_crypto:
        exclude = CRYPTO_EXCLUDE
        print("excluded benches that should implemented on dedicated circuits (e.g. AES, JPEG.)")
    if args.exclude:
        exclude += args.exclude
        print(f"args.excluded: {sorted(args.exclude)}")

    per_bench, combined = load_results(args.result, exclude=exclude)
    if not per_bench:
        print(f"No CSV files found in {args.result}", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(per_bench)} benchmark(s) from {args.result}\n")
    print_summary_table(combined)

    plot_cumulative_overall(combined, os.path.join(args.output, "cumulative_overall.png"))
    plot_class_totals(combined, os.path.join(args.output, "class_totals.png"))
    plot_benchmark_totals(per_bench, os.path.join(args.output, "benchmark_totals.png"))
    plot_class_histograms(
        combined, os.path.join(args.output, "histogram_by_class.png"),
        title="Effective bit-width distribution by instruction class",
        subtitle=f"all {len(per_bench)} benchmark(s) combined")

    plot_class_histograms(
        aggregate_by_benchmark(per_bench), os.path.join(args.output, "histogram_by_benches.png"),
        title="Effective bit-width distribution by benchmark",
        subtitle="all instruction classes combined")

    by_bench_dir = None
    if args.exclude_crypto:
        by_bench_dir = os.path.join(args.output, "by_benchmark")
        os.makedirs(by_bench_dir, exist_ok=True)
        for name, table in per_bench.items():
            plot_class_histograms(
                table, os.path.join(by_bench_dir, f"{name}.png"),
                title=f"Effective bit-width distribution by instruction class -- {name}")

    print(f"\nCharts written to {args.output}/ (per-benchmark grids in {by_bench_dir if by_bench_dir is not None else ""}/)")


if __name__ == "__main__":
    main()
