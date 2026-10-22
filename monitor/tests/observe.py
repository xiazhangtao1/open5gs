"""Record local lab source cadence and bounded process resources during k6 runs."""
import argparse
import json
from pathlib import Path
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=int, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--pid', type=int, action='append', default=[])
args = parser.parse_args()
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def counts():
    with opener.open('http://127.0.0.1:19090/counts', timeout=1) as response:
        return json.load(response)


def resources():
    result = {}
    for pid in args.pid:
        data = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines())
        stat = Path(f'/proc/{pid}/stat').read_text().split()
        try:
            fds = len(list(Path(f'/proc/{pid}/fd').iterdir()))
        except PermissionError:
            fds = None  # A non-root host observer cannot inspect another container UID's FDs.
        result[str(pid)] = dict(rss_kib=int(data['VmRSS'].split()[0]),
                                hwm_kib=int(data['VmHWM'].split()[0]), threads=int(data['Threads']),
                                cpu_ticks=int(stat[13]) + int(stat[14]),
                                fds=fds)
    return result


started = time.time()
before = counts()
samples = []
while True:
    elapsed = time.time() - started
    samples.append(dict(elapsed=elapsed, resources=resources(), counts=counts()))
    if elapsed >= args.seconds:
        break
    time.sleep(min(15, args.seconds - elapsed))
after = counts()
result = dict(seconds=time.time() - started, before=before, after=after,
              per_second={key: (after.get(key, 0) - before.get(key, 0)) / (time.time() - started) for key in after}, samples=samples)
args.output.write_text(json.dumps(result, indent=2))
print(json.dumps({key: value for key, value in result.items() if key != 'samples'}, indent=2))
