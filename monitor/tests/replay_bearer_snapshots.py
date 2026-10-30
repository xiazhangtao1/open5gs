"""Replay captured isolated UPF snapshots and independently verify bearer histories.

Usage: python3 monitor/tests/replay_bearer_snapshots.py /path/to/snapshots.jsonl
The capture must come from an isolated protocol test, not synthetic rate rows.
"""
import json
from pathlib import Path
import sys
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import Cache
from memory import MemoryBudget, MIB
from server import validate_snapshot

c = Cache(MemoryBudget(limit=512 * MIB, usage=lambda: 32 * MIB))
count = checked = positive = simultaneous = resets = 0
qfis = set()
epochs = {}
for line in Path(sys.argv[1]).read_text().splitlines():
    d = json.loads(line)
    stamp = int(d['timestamp_us']) / 1e6
    with patch('server.time.time', return_value=stamp):
        validate_snapshot(d)
    c.replace('upf_rates', d['sessions'], 2)
    c.record_rates(stamp, d['boot_id'])
    expected = {}
    for session in d['sessions']:
        for rule in session['rules']:
            if not rule['qfi']:
                continue
            key = f"bearer:{session['seid']}:{rule['qfi']}"
            pair = expected.setdefault(key, [0, 0])
            pair[0 if rule['direction'] == 'UL' else 1] += int(rule['bps'])
            qfis.add(rule['qfi'])
    simultaneous = max(simultaneous, len(expected))
    for key, pair in expected.items():
        with patch('cache.time.time', return_value=stamp):
            result = c.query('trends', dict(key=key, seconds='1800'))
        assert result['history_state'] == 'recording', result
        assert result['points'][-1] == [stamp, *pair], (key, pair, result['points'][-1])
        if key in epochs and epochs[key] != result['epoch']:
            resets += 1
        epochs[key] = result['epoch']
        checked += 1
        positive += int(any(pair))
    count += 1
assert count >= 30 and checked >= 50 and positive >= 20 and len(qfis) >= 3 and simultaneous >= 2
print(json.dumps(dict(snapshots=count, bearer_samples=checked, positive_samples=positive,
                     distinct_qfis=sorted(qfis), max_simultaneous=simultaneous, epoch_resets=resets,
                     trend_bytes=c.trend_bytes(), bearer_bytes=c.bearer_bytes())))
