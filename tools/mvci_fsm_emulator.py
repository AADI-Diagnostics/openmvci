"""Deterministic emulator scaffold driven by usb-msvc32 FSM JSON.

This utility reads the FSM file produced by analyze_usb_trace.py and offers:
- Interactive stdin REPL: one OUT hex frame per line.
- Single-shot mode via --input.

For each accepted OUT frame, it returns the expected IN hex frame and updates state.
"""

from __future__ import annotations

import argparse
import json
import sys
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

    @staticmethod
    def normalize_hex(text: str) -> str:
        return "".join(ch for ch in text.strip().lower() if ch in "0123456789abcdef")

    def step(self, out_hex: str) -> dict:
        norm = self.normalize_hex(out_hex)
        if not norm:
            return {
                "ok": False,
                "state": self.state,
                "error": "empty input",
            }

        matches = [
            t
            for t in self.transitions
            if t.src == self.state and t.out_hex == norm
        ]

        if not matches:
            # Fallback: allow steady transitions while in non-initial states.
            # This captures rare branches that appear between startup phases.
            if self.state != "S0":
                matches = [
                    t
                    for t in self.transitions
                    if t.src == "S_READY" and t.out_hex == norm
                ]

        if not matches:
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
    parser = argparse.ArgumentParser(description="Replay FSM deterministic emulator")
    parser.add_argument("--fsm", required=True, help="Path to usb-msvc32_fsm.generated.json")
    parser.add_argument(
        "--initial-state",
        required=False,
        help="Override initial state (e.g. S_READY for one-shot steady command tests)",
    )
    parser.add_argument(
        "--input",
        required=False,
        help="Optional single OUT hex frame to process once",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print response as JSON object",
    )
    return parser.parse_args()


def load_emulator(fsm_path: Path) -> FsmEmulator:
    doc = json.loads(fsm_path.read_text())
    initial_state = doc.get("initial_state", "S0")
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


def format_response(resp: dict, as_json: bool) -> str:
    if as_json:
        return json.dumps(resp)

    if not resp.get("ok"):
        return f"ERR state={resp.get('state')} reason={resp.get('error')}"

    reply = resp.get("reply") or ""
    return (
        f"OK state={resp.get('state')} type={resp.get('type')} "
        f"reply={reply}"
    )


def run_repl(emu: FsmEmulator, as_json: bool) -> int:
    print("mvci fsm emulator ready; enter OUT hex frame per line; Ctrl-D to exit")
    for line in sys.stdin:
        text = line.strip()
        if not text:
            continue
        resp = emu.step(text)
        print(format_response(resp, as_json))
    return 0


def main() -> int:
    args = parse_args()
    emu = load_emulator(Path(args.fsm))
    if args.initial_state:
        emu.state = args.initial_state

    if args.input:
        resp = emu.step(args.input)
        print(format_response(resp, args.json))
        return 0 if resp.get("ok") else 2

    return run_repl(emu, args.json)


if __name__ == "__main__":
    raise SystemExit(main())
