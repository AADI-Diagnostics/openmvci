"""Replay OUT frames from a trace CSV against FSM and compare expected vs observed IN.

Input CSV format is the analyzer export with columns:
index,t_ms,direction,len,hex,ascii
"""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Transition:
    src: str
    dst: str
    out_hex: str
    expected_in: str | None
    observed_count: int
    transition_type: str


class FsmEmulator:
    def __init__(self, initial_state: str, transitions: list[Transition]) -> None:
        self.state = initial_state
        self.transitions = transitions
        self.class_fallback: dict[str, str] = {}

        # Build dominant reply per class from steady transitions.
        class_counts: dict[str, dict[str, int]] = {}
        for t in transitions:
            # Class info is encoded in transition_type for startup vs steady;
            # actual class tag is unavailable here, so infer from length.
            if t.transition_type != "steady" or not t.expected_in:
                continue
            frame_len = len(t.out_hex) // 2
            if frame_len == 35:
                cls = "cmd35"
            elif frame_len == 27:
                cls = "cmd27"
            elif frame_len == 11:
                cls = "cmd11"
            else:
                continue
            class_counts.setdefault(cls, {})
            class_counts[cls][t.expected_in] = class_counts[cls].get(t.expected_in, 0) + t.observed_count

        for cls, counts in class_counts.items():
            self.class_fallback[cls] = max(counts.items(), key=lambda kv: kv[1])[0]

    @staticmethod
    def normalize_hex(text: str) -> str:
        return "".join(ch for ch in text.strip().lower() if ch in "0123456789abcdef")

    def step(self, out_hex: str) -> dict:
        norm = self.normalize_hex(out_hex)
        if not norm:
            return {"ok": False, "state": self.state, "error": "empty input"}

        matches = [
            t for t in self.transitions if t.src == self.state and t.out_hex == norm
        ]
        if not matches and self.state == "S_READY":
            matches = [
                t for t in self.transitions if t.src == "S_READY" and t.out_hex == norm
            ]

        if not matches:
            # Exact signature fallback via S_READY for non-initial states.
            if self.state != "S0":
                ready_exact = [
                    t for t in self.transitions if t.src == "S_READY" and t.out_hex == norm
                ]
                if ready_exact:
                    transition = max(ready_exact, key=lambda t: t.observed_count)
                    self.state = transition.dst
                    return {
                        "ok": True,
                        "state": self.state,
                        "reply": transition.expected_in,
                        "type": "steady-ready-exact",
                        "observed_count": transition.observed_count,
                        "input": norm,
                    }

            # Class-based fallback in non-initial states for unseen variants.
            if self.state != "S0":
                frame_len = len(norm) // 2
                cls = None
                if frame_len == 35:
                    cls = "cmd35"
                elif frame_len == 27:
                    cls = "cmd27"
                elif frame_len == 11:
                    cls = "cmd11"

                if cls and cls in self.class_fallback:
                    return {
                        "ok": True,
                        "state": "S_READY",
                        "reply": self.class_fallback[cls],
                        "type": "steady-fallback",
                        "observed_count": 0,
                        "input": norm,
                    }

            return {
                "ok": False,
                "state": self.state,
                "error": "no transition",
                "input": norm,
            }

        transition = max(matches, key=lambda t: t.observed_count)
        self.state = transition.dst
        return {
            "ok": True,
            "state": self.state,
            "reply": transition.expected_in,
            "type": transition.transition_type,
            "observed_count": transition.observed_count,
            "input": norm,
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Replay trace against FSM profile")
    parser.add_argument("--fsm", required=True, help="Path to FSM JSON")
    parser.add_argument("--csv", required=True, help="Path to bulk trace CSV")
    parser.add_argument(
        "--pair-window-ms",
        type=float,
        default=0.2,
        help="Window for matching OUT to nearest following IN",
    )
    parser.add_argument(
        "--initial-state",
        default="S0",
        help="Initial FSM state override",
    )
    parser.add_argument(
        "--out-json",
        required=False,
        help="Optional output JSON report path",
    )
    parser.add_argument(
        "--out-md",
        required=False,
        help="Optional output markdown report path",
    )
    return parser.parse_args()


def load_fsm(path: Path, initial_override: str) -> FsmEmulator:
    doc = json.loads(path.read_text())
    initial_state = initial_override or doc.get("initial_state", "S0")
    transitions = []
    for item in doc.get("transitions", []):
        transitions.append(
            Transition(
                src=item["from"],
                dst=item["to"],
                out_hex=item["out"].lower(),
                expected_in=item.get("expected_in"),
                observed_count=int(item.get("observed_count", 0)),
                transition_type=item.get("type", "steady"),
            )
        )
    return FsmEmulator(initial_state, transitions)


def load_rows(path: Path) -> list[dict]:
    rows = []
    with path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                {
                    "index": int(row["index"]),
                    "t_ms": float(row["t_ms"]),
                    "direction": row["direction"],
                    "hex": row["hex"].lower(),
                    "len": int(row["len"]),
                }
            )
    return rows


def candidate_ins(rows: list[dict], out_index: int, pair_window_ms: float) -> list[dict]:
    """Return IN frames seen shortly after a given OUT row index."""
    out_row = rows[out_index]
    items = []
    j = out_index + 1
    while j < len(rows) and rows[j]["t_ms"] - out_row["t_ms"] <= pair_window_ms:
        if rows[j]["direction"] == "IN":
            items.append(rows[j])
        j += 1
    return items


def main() -> int:
    args = parse_args()
    emu = load_fsm(Path(args.fsm), args.initial_state)
    rows = load_rows(Path(args.csv))

    # Detect startup stage-1 trigger from transitions: S0 -> S_START1_OK.
    startup1 = None
    for t in emu.transitions:
        if t.src == "S0" and t.dst == "S_START1_OK":
            startup1 = t.out_hex
            break

    results = []
    ok_transition = 0
    matched_reply = 0
    mismatched_reply = 0
    missing_observed_reply = 0
    unknown_transition = 0

    for i, row in enumerate(rows):
        if row["direction"] != "OUT":
            continue
        out_row = row

        # Auto-reset FSM when a new startup sequence begins mid-trace.
        if startup1 and out_row["hex"] == startup1 and emu.state == "S_READY":
            emu.state = "S0"

        step = emu.step(out_row["hex"])
        # If startup appears out-of-state, force reset and retry once.
        if (
            not step.get("ok")
            and startup1
            and out_row["hex"] == startup1
            and emu.state != "S0"
        ):
            emu.state = "S0"
            step = emu.step(out_row["hex"])

        expected = step.get("reply") if step.get("ok") else None

        # Prefer expected reply if present in window to avoid pairing to short poll ACKs.
        ins = candidate_ins(rows, i, args.pair_window_ms)
        observed = None
        if expected:
            for in_row in ins:
                if in_row["hex"] == expected:
                    observed = in_row["hex"]
                    break
        if observed is None and ins:
            observed = ins[0]["hex"]

        if step.get("ok"):
            ok_transition += 1
            if expected is None:
                pass
            elif observed is None:
                missing_observed_reply += 1
            elif expected == observed:
                matched_reply += 1
            else:
                mismatched_reply += 1
        else:
            unknown_transition += 1

        results.append(
            {
                "out_index": out_row["index"],
                "t_ms": out_row["t_ms"],
                "state_after": step.get("state"),
                "transition_ok": bool(step.get("ok")),
                "out": out_row["hex"],
                "expected_in": expected,
                "observed_in": observed,
                "match": expected is not None and observed is not None and expected == observed,
                "error": step.get("error"),
            }
        )

    total_out = sum(1 for r in rows if r["direction"] == "OUT")
    reply_comparable = matched_reply + mismatched_reply
    reply_accuracy = (matched_reply / reply_comparable) if reply_comparable else 0.0

    summary = {
        "total_out": total_out,
        "ok_transition": ok_transition,
        "unknown_transition": unknown_transition,
        "matched_reply": matched_reply,
        "mismatched_reply": mismatched_reply,
        "missing_observed_reply": missing_observed_reply,
        "reply_accuracy": reply_accuracy,
        "pair_window_ms": args.pair_window_ms,
    }

    report = {
        "summary": summary,
        "first_mismatches": [r for r in results if not r["transition_ok"] or (r["expected_in"] and r["observed_in"] and not r["match"])][:50],
    }

    if args.out_json:
        Path(args.out_json).write_text(json.dumps(report, indent=2))

    if args.out_md:
        lines = []
        lines.append("# trace vs FSM report")
        lines.append("")
        lines.append(f"- total_out: {summary['total_out']}")
        lines.append(f"- ok_transition: {summary['ok_transition']}")
        lines.append(f"- unknown_transition: {summary['unknown_transition']}")
        lines.append(f"- matched_reply: {summary['matched_reply']}")
        lines.append(f"- mismatched_reply: {summary['mismatched_reply']}")
        lines.append(f"- missing_observed_reply: {summary['missing_observed_reply']}")
        lines.append(f"- reply_accuracy: {summary['reply_accuracy']:.4f}")
        lines.append("")
        lines.append("## first mismatches")
        lines.append("")
        lines.append("| out_index | t_ms | out | expected_in | observed_in | error |")
        lines.append("|---|---|---|---|---|---|")
        for item in report["first_mismatches"]:
            lines.append(
                "| "
                f"{item['out_index']} | {item['t_ms']:.6f} | {item['out'][:48]}... | "
                f"{(item['expected_in'] or '')[:48]}... | {(item['observed_in'] or '')[:48]}... | {item['error'] or ''} |"
            )
        Path(args.out_md).write_text("\n".join(lines))

    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
