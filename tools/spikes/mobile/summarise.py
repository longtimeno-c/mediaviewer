#!/usr/bin/env python3
# M1 spikes (throwaway). Turns results.jsonl into the rows RESULTS-TEMPLATE.md asks for.
#   python3 summarise.py results-iphone.jsonl
import json
import sys


def rows(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                yield json.loads(line[line.find("{"):])


def main():
    results = list(rows(sys.argv[1]))
    if not results:
        sys.exit("no results in that file")
    first = results[0]
    print(f"Device {first['device']}, iOS {first['ios']}, simulator: {first['simulator']}\n")

    print("| S1 launch | prewarmed | threads | uptime s | process age at start ms | load ms | first encode ms | didFinish to first encode ms | process start to first encode ms | p50 ms | p95 ms | MB after load | available MB after load | MB peak | MB after all | thermal |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        if r["spike"] == "s1" and "error" not in r:
            print(f"| {r['trigger']} | {r['prewarmed']} | {r['threads']} | {r['uptime_s']} | {r['process_age_at_start_ms']} | "
                  f"{r['load_ms']} | {r['first_encode_ms']} | {r['did_finish_to_first_encode_ms']} | "
                  f"{r['process_start_to_first_encode_ms']} | {r['encode_p50_ms']} | {r['encode_p95_ms']} | "
                  f"{r['mb_after_load']} | {r['available_mb_after_load']} | {r['mb_peak_after_load']} | {r['mb_after_all']} | {r['thermal']} |")

    print("\n| S3 mode | rows | kernel | threads | first pass ms | p50 ms | max ms | MB after load | MB after scan | MB peak | available MB | thermal |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        if r["spike"] == "s3" and "error" not in r:
            print(f"| {r['mode']} | {r['rows']} | {r['kernel']} | {r['threads']} | {r['pass_first_ms']} | {r['pass_p50_ms']} | "
                  f"{r['pass_max_ms']} | {r['mb_after_load']} | {r['mb_after_scan']} | {r['mb_peak_after_scan']} | {r['available_mb']} | {r['thermal']} |")

    print("\n| S4 source | ready ms | first frame ms | seek to playing p50 / max ms | seeks ok | stalls | no-seek played / wall s | waiting ms (events) | limited by clip | clip Mbit/s | bytes served | requests | read p50 / p95 ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        if r["spike"] == "s4" and "error" not in r:
            print(f"| {r['source']} | {r['ready_ms']} | {r['first_frame_ms']} | {r['seek_to_playing_p50_ms']} / {r['seek_to_playing_max_ms']} | "
                  f"{r['seeks_ok']} of {len(r['seeks'])} | {r['stalls']} | {r['sustain_played_s']} / {r['sustain_wall_s']} | "
                  f"{r['sustain_waiting_ms']} ({r['sustain_waiting_events']}) | {r['sustain_limited_by_clip']} | {r['clip_mbit']} | "
                  f"{r['bytes_served']} | {r['loader_requests']} | "
                  f"{r['read_p50_ms']} / {r['read_p95_ms']} |")

    print("\n| Socket probe | host | DNS64 | resolve ms | ok | answered over IPv | families tried | connect ms | first byte ms |")
    print("|---|---|---|---|---|---|---|---|---|")
    for r in results:
        if r["spike"] == "s2net" and "error" not in r:
            tried = r["attempts"]
            families = ", ".join(f"{a['family']}{' (well-known NAT64)' if a['well_known_nat64'] else ''}{': ' + a['error'] if a['error'] else ''}" for a in tried)
            last = tried[-1] if tried else {}
            print(f"| {r['mode']} | {r['host_kind']} | {r['dns64']} | {r.get('resolve_ms', 'n/a')} | {r['ok']} | {r['answered_family']} | "
                  f"{families or r.get('gai_error')} | {last.get('connect_ms', 'n/a')} | {last.get('first_byte_ms', 'n/a')} |")

    problems = [r for r in results if r["spike"] == "killed" or "error" in r]
    print("\nKilled or failed:" if problems else "\nNothing killed or failed.")
    for r in problems:
        print(f"- {r['spike']} at {r['at']}: {r.get('during') or r.get('error')}")


if __name__ == "__main__":
    main()
