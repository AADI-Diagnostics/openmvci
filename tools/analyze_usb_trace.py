"""Analyze USB bulk trace CSV exported from usb-msvc32 capture parsing.

This script consumes the csv produced by prior extraction steps and writes:
- A replay plan markdown file with dominant request/response signatures.
- A JSON map with structured message classes and cycle details.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


STARTUP_TRIPLET = [
	"030003",
	"0c000700014d5643492d5462",
	"1300d04d01f77639076b2740ea48fd6ea4a900",
]


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Analyze USB bulk trace CSV for replay mapping")
	parser.add_argument("--csv", required=True, help="Input csv path (usb-msvc32_bulk_trace.csv)")
	parser.add_argument("--out-md", required=True, help="Output markdown replay plan path")
	parser.add_argument("--out-json", required=True, help="Output JSON replay map path")
	parser.add_argument(
		"--out-fsm-json",
		required=False,
		help="Optional emulator-oriented FSM json output path",
	)
	parser.add_argument(
		"--compare-json",
		required=False,
		help="Optional baseline replay-map JSON to compare against",
	)
	parser.add_argument(
		"--out-compare-md",
		required=False,
		help="Optional markdown output path for comparison report",
	)
	parser.add_argument(
		"--pair-window-ms",
		type=float,
		default=0.2,
		help="Max time delta in ms to pair OUT frame with next IN frame",
	)
	parser.add_argument(
		"--fsm-max-steady",
		type=int,
		default=200,
		help="Max number of steady-state OUT signatures to emit into FSM",
	)
	return parser.parse_args()


def load_rows(path: Path) -> list[dict]:
	rows = []
	with path.open() as f:
		reader = csv.DictReader(f)
		for row in reader:
			rows.append(
				{
					"idx": int(row["index"]),
					"t": float(row["t_ms"]),
					"dir": row["direction"],
					"hex": row["hex"],
					"b": bytes.fromhex(row["hex"]),
					"len": int(row["len"]),
				}
			)
	return rows


def detect_cycle_starts(out_rows: list[dict]) -> list[float]:
	starts = []
	for i in range(len(out_rows) - 2):
		if (
			out_rows[i]["hex"] == STARTUP_TRIPLET[0]
			and out_rows[i + 1]["hex"] == STARTUP_TRIPLET[1]
			and out_rows[i + 2]["hex"] == STARTUP_TRIPLET[2]
		):
			starts.append(out_rows[i]["t"])
	return starts


def cycle_windows(cycle_starts: list[float], end_ms: float) -> list[tuple[int, float, float]]:
	windows = []
	for i, start in enumerate(cycle_starts):
		stop = cycle_starts[i + 1] if i + 1 < len(cycle_starts) else end_ms
		windows.append((i + 1, start, stop))
	return windows


def pair_out_in(rows: list[dict], window_ms: float) -> list[tuple[dict, dict, float]]:
	pairs = []
	for i, row in enumerate(rows):
		if row["dir"] != "OUT":
			continue
		j = i + 1
		while j < len(rows) and rows[j]["t"] - row["t"] <= window_ms:
			if rows[j]["dir"] == "IN":
				pairs.append((row, rows[j], rows[j]["t"] - row["t"]))
				break
			j += 1
	return pairs


def classify_out(hex_frame: str) -> dict:
	frame = bytes.fromhex(hex_frame)
	info = {"len": len(frame), "hex": hex_frame}

	if len(frame) == 35:
		info["class"] = "cmd35"
		info["desc"] = "35-byte secured command block"
		info["fields"] = {
			"len_le16": hex_frame[:4],
			"msg_nonce_candidate": hex_frame[4:20],
			"session_seed_candidate": hex_frame[20:32],
			"payload_auth_candidate": hex_frame[32:],
		}
	elif len(frame) == 27:
		info["class"] = "cmd27"
		info["desc"] = "27-byte mid-stage command"
		info["fields"] = {
			"len_le16": hex_frame[:4],
			"const_prefix": hex_frame[4:12],
			"const_word": hex_frame[12:20],
			"tail_variable": hex_frame[20:],
		}
	elif len(frame) == 19:
		info["class"] = "start3"
		info["desc"] = "startup/auth stage 3"
	elif len(frame) == 12:
		info["class"] = "start2"
		info["desc"] = "startup/auth stage 2 (contains MVCI-Tb)"
	elif len(frame) == 3:
		info["class"] = "start1"
		info["desc"] = "startup/auth stage 1"
	elif len(frame) == 11:
		info["class"] = "cmd11"
		info["desc"] = "poll/keepalive and small control command family"
		info["fields"] = {
			"prefix4": hex_frame[:8],
			"tail": hex_frame[8:],
		}
	else:
		info["class"] = "other"
		info["desc"] = "other"

	return info


def build_fsm(
	out_hex_counts: Counter,
	out_to_reply: dict,
	cycle_details: list[dict],
	fsm_max_steady: int,
) -> dict:
	"""Build a minimal replay state machine suitable for emulator scaffolding."""

	transitions = []
	states = ["S0", "S_START1_OK", "S_START2_OK", "S_READY"]

	# Deterministic startup transitions.
	startup_states = ["S0", "S_START1_OK", "S_START2_OK"]
	startup_targets = ["S_START1_OK", "S_START2_OK", "S_READY"]
	for i, out_hex in enumerate(STARTUP_TRIPLET):
		expected_in = None
		hits = 0
		if out_hex in out_to_reply and out_to_reply[out_hex]:
			expected_in, hits = out_to_reply[out_hex].most_common(1)[0]
		transitions.append(
			{
				"from": startup_states[i],
				"to": startup_targets[i],
				"out": out_hex,
				"expected_in": expected_in,
				"observed_count": hits,
				"type": "startup",
			}
		)

	# Ready-state transitions for dominant commands.
	for out_hex, total in out_hex_counts.most_common(max(1, fsm_max_steady)):
		if out_hex in STARTUP_TRIPLET:
			continue
		expected_in = None
		hits = 0
		if out_hex in out_to_reply and out_to_reply[out_hex]:
			expected_in, hits = out_to_reply[out_hex].most_common(1)[0]
		transitions.append(
			{
				"from": "S_READY",
				"to": "S_READY",
				"out": out_hex,
				"expected_in": expected_in,
				"observed_count": hits,
				"coverage_ratio": (hits / total) if total else 0.0,
				"class": classify_out(out_hex)["class"],
				"type": "steady",
			}
		)

	return {
		"states": states,
		"initial_state": "S0",
		"transitions": transitions,
		"cycle_profile": [
			{
				"cycle": cycle["cycle"],
				"cmd35_session_seed_candidates": cycle["cmd35_session_seed_candidates"],
				"cmd35_payload_lead2": cycle["cmd35_payload_lead2"],
			}
			for cycle in cycle_details
		],
	}


def compare_maps(current: dict, baseline: dict) -> dict:
	"""Return a compact drift assessment between two replay maps."""

	checks = []

	startup_match = current.get("startup_triplet", []) == baseline.get("startup_triplet", [])
	checks.append(
		{
			"name": "startup_triplet",
			"passed": startup_match,
			"detail": "exact match" if startup_match else "differs",
		}
	)

	cycles_cur = len(current.get("cycle_starts_ms", []))
	cycles_base = len(baseline.get("cycle_starts_ms", []))
	cycle_pass = cycles_cur == cycles_base
	checks.append(
		{
			"name": "cycle_count",
			"passed": cycle_pass,
			"detail": f"current={cycles_cur} baseline={cycles_base}",
		}
	)

	cur_pairs = {(x["out"], x["in"]): x["count"] for x in current.get("out_to_in_top", [])[:15]}
	base_pairs = {(x["out"], x["in"]): x["count"] for x in baseline.get("out_to_in_top", [])[:15]}
	overlap = len(set(cur_pairs).intersection(set(base_pairs)))
	pair_pass = overlap >= 10
	checks.append(
		{
			"name": "top_pair_overlap",
			"passed": pair_pass,
			"detail": f"overlap={overlap} of 15",
		}
	)

	cur_top = current.get("out_to_in_top", [{}])[0]
	base_top = baseline.get("out_to_in_top", [{}])[0]
	top_pass = cur_top.get("out") == base_top.get("out") and cur_top.get("in") == base_top.get("in")
	checks.append(
		{
			"name": "dominant_mapping",
			"passed": top_pass,
			"detail": (
				"same top out->in"
				if top_pass
				else f"current={cur_top.get('out')}=>{cur_top.get('in')} baseline={base_top.get('out')}=>{base_top.get('in')}"
			),
		}
	)

	passed = sum(1 for c in checks if c["passed"])
	score = passed / len(checks) if checks else 0.0
	status = "compatible" if score >= 0.75 else "drift-detected"

	return {
		"status": status,
		"score": score,
		"checks": checks,
	}


def main() -> int:
	args = parse_args()
	csv_path = Path(args.csv)
	out_md = Path(args.out_md)
	out_json = Path(args.out_json)

	rows = load_rows(csv_path)
	if not rows:
		raise SystemExit("No rows loaded from CSV")

	out_rows = [r for r in rows if r["dir"] == "OUT"]
	cycle_starts = detect_cycle_starts(out_rows)
	windows = cycle_windows(cycle_starts, rows[-1]["t"] + 1.0)
	pairs = pair_out_in(rows, args.pair_window_ms)

	map_count = Counter((a["hex"], b["hex"]) for a, b, _ in pairs)
	out_to_reply = defaultdict(Counter)
	for out_row, in_row, _ in pairs:
		out_to_reply[out_row["hex"]][in_row["hex"]] += 1

	out_hex_counts = Counter(r["hex"] for r in out_rows)

	message_classes = []
	for hex_frame, count in out_hex_counts.most_common():
		cls = classify_out(hex_frame)
		cls["count"] = count
		message_classes.append(cls)

	cycle_details = []
	for cycle_id, start, stop in windows:
		out_win = [r for r in out_rows if start <= r["t"] < stop]
		cmd35 = [r for r in out_win if r["len"] == 35]
		cmd27 = [r for r in out_win if r["len"] == 27]
		cycle_details.append(
			{
				"cycle": cycle_id,
				"start_ms": start,
				"end_ms": stop,
				"out_count": len(out_win),
				"cmd35_count": len(cmd35),
				"cmd27_count": len(cmd27),
				"cmd35_session_seed_candidates": sorted({r["hex"][20:32] for r in cmd35}),
				"cmd35_payload_lead2": sorted({r["hex"][32:36] for r in cmd35}),
			}
		)

	structured = {
		"source": str(csv_path),
		"pair_window_ms": args.pair_window_ms,
		"startup_triplet": STARTUP_TRIPLET,
		"cycle_starts_ms": cycle_starts,
		"cycle_details": cycle_details,
		"message_classes": message_classes,
		"out_to_in_top": [
			{"out": out_hex, "in": in_hex, "count": count}
			for (out_hex, in_hex), count in map_count.most_common(30)
		],
	}
	out_json.write_text(json.dumps(structured, indent=2))

	if args.out_fsm_json:
		fsm = build_fsm(out_hex_counts, out_to_reply, cycle_details, args.fsm_max_steady)
		Path(args.out_fsm_json).write_text(json.dumps(fsm, indent=2))

	lines = []
	lines.append("# usb-msvc32 replay candidate plan")
	lines.append("")
	lines.append("## Deterministic startup sequence")
	lines.append("Send these OUT frames in order and expect corresponding IN replies:")
	lines.append("")
	lines.append("| Step | OUT frame | Expected IN reply | Seen count |")
	lines.append("|---|---|---|---|")
	for out_hex in STARTUP_TRIPLET:
		if out_hex in out_to_reply and out_to_reply[out_hex]:
			in_hex, count = out_to_reply[out_hex].most_common(1)[0]
			lines.append(f"| startup | `{out_hex}` | `{in_hex}` | {count} |")
		else:
			lines.append(f"| startup | `{out_hex}` | (none in pair window) | 0 |")

	lines.append("")
	lines.append("## Dominant command classes after startup")
	lines.append("")
	lines.append("| OUT class | OUT frame signature | Dominant IN reply | Matches | Notes |")
	lines.append("|---|---|---|---|---|")
	for out_hex, count in out_hex_counts.most_common(12):
		dominant_reply = "(none)"
		match_count = 0
		if out_hex in out_to_reply and out_to_reply[out_hex]:
			dominant_reply, match_count = out_to_reply[out_hex].most_common(1)[0]

		short_out = out_hex if len(out_hex) <= 50 else out_hex[:50] + "..."
		short_in = dominant_reply if len(dominant_reply) <= 50 else dominant_reply[:50] + "..."
		cls = classify_out(out_hex)
		lines.append(
			f"| {cls['class']} | `{short_out}` | `{short_in}` | {match_count} | {cls['desc']} |"
		)

	lines.append("")
	lines.append("## Cycle-dependent fields for 35-byte commands")
	lines.append("")
	lines.append("| Cycle | Time window (ms) | 35B count | Session-seed candidate ([10:16]) | Lead2 ([16:18]) |")
	lines.append("|---|---|---|---|---|")
	for cycle in cycle_details:
		seed = ", ".join(f"`{x}`" for x in cycle["cmd35_session_seed_candidates"])
		lead = ", ".join(f"`{x}`" for x in cycle["cmd35_payload_lead2"])
		lines.append(
			f"| {cycle['cycle']} | {cycle['start_ms']:.3f}-{cycle['end_ms']:.3f} | {cycle['cmd35_count']} | {seed} | {lead} |"
		)

	lines.append("")
	lines.append("## Replay strategy suggestion")
	lines.append("1. Execute startup triplet and verify expected IN signatures before sending 35-byte commands.")
	lines.append("2. Treat 35-byte commands as session-bound; avoid replay across cycles without field regeneration.")
	lines.append("3. Use the dominant 11-byte command family for keepalive behavior after startup converges.")
	lines.append("4. Start from exact signature replay, then replace variable fields one region at a time.")
	lines.append("")
	lines.append("## Caveat")
	lines.append("This is a behavioral map from one trace, not a proven cryptographic derivation.")

	if args.compare_json:
		baseline = json.loads(Path(args.compare_json).read_text())
		comparison = compare_maps(structured, baseline)
		lines.append("")
		lines.append("## Baseline Compatibility")
		lines.append(f"Status: **{comparison['status']}**")
		lines.append(f"Score: {comparison['score']:.2f}")
		lines.append("")
		for check in comparison["checks"]:
			mark = "PASS" if check["passed"] else "FAIL"
			lines.append(f"- {mark} {check['name']}: {check['detail']}")

		if args.out_compare_md:
			compare_lines = ["# usb-msvc32 baseline comparison"]
			compare_lines.append("")
			compare_lines.append(f"Status: **{comparison['status']}**")
			compare_lines.append(f"Score: {comparison['score']:.2f}")
			compare_lines.append("")
			for check in comparison["checks"]:
				mark = "PASS" if check["passed"] else "FAIL"
				compare_lines.append(f"- {mark} {check['name']}: {check['detail']}")
			Path(args.out_compare_md).write_text("\n".join(compare_lines))
	out_md.write_text("\n".join(lines))

	print(f"Wrote {out_md}")
	print(f"Wrote {out_json}")
	if args.out_fsm_json:
		print(f"Wrote {args.out_fsm_json}")
	if args.out_compare_md and args.compare_json:
		print(f"Wrote {args.out_compare_md}")
	print(f"Detected cycles: {len(cycle_starts)}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
