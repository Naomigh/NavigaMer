#!/usr/bin/env python3
"""Generate deterministic reference-derived and no-hit FASTA queries."""

import argparse
import random


def read_fasta(path: str) -> str:
    parts = []
    active = False
    with open(path, encoding="ascii") as handle:
        for line in handle:
            line = line.strip()
            if line.startswith(">"):
                if active and parts:
                    break
                active = True
            elif active:
                parts.append(line.upper())
    return "".join(parts)


def mutate(sequence: str, edits: int, rng: random.Random) -> tuple[str, str]:
    value = list(sequence)
    operations = []
    alphabet = "ACGT"
    for step in range(edits):
        kind = step % 3
        if kind == 0 or not value:
            position = rng.randrange(len(value))
            choices = [base for base in alphabet if base != value[position]]
            value[position] = rng.choice(choices)
            operations.append(f"S{position}")
        elif kind == 1:
            position = rng.randrange(len(value) + 1)
            value.insert(position, rng.choice(alphabet))
            operations.append(f"I{position}")
        else:
            position = rng.randrange(len(value))
            value.pop(position)
            operations.append(f"D{position}")
    return "".join(value), ",".join(operations) or "none"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--window", type=int, default=150)
    parser.add_argument("--reference-windows", type=int, default=10000)
    parser.add_argument("--max-edits", type=int, default=5)
    parser.add_argument("--seed", type=int, default=20260914)
    parser.add_argument("--mode", choices=("ordered", "shuffled", "repeated"),
                        default="ordered")
    parser.add_argument("--no-hit-every", type=int, default=10)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    reference = read_fasta(args.reference)
    available = min(args.reference_windows, len(reference) - args.window + 1)
    records = []
    for ordinal in range(args.count):
        if args.no_hit_every and ordinal % args.no_hit_every == args.no_hit_every - 1:
            sequence = "".join(rng.choice("ACGT") for _ in range(args.window))
            records.append((f"q{ordinal}_random_nohit", sequence))
            continue
        if args.mode == "repeated":
            start = min(available - 1, available // 2)
            edits = 0
        else:
            # Preserve adjacent-reference locality before the optional shuffle.
            start = ordinal % available
            edits = ordinal % (args.max_edits + 1)
        sequence, operations = mutate(
            reference[start:start + args.window], edits, rng)
        records.append((f"q{ordinal}_source{start}_steps{edits}_{operations}", sequence))
    if args.mode == "shuffled":
        rng.shuffle(records)
    with open(args.output, "w", encoding="ascii") as handle:
        for name, sequence in records:
            handle.write(f">{name}\n{sequence}\n")


if __name__ == "__main__":
    main()
