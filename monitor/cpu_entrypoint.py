"""Wait for this container's CPU allocation, then exec without changing quotas."""
import os
import sys

from common import wait_for_cpu


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit('cpu_entrypoint.py requires a command')
    wait_for_cpu()
    os.execvp(sys.argv[1], sys.argv[1:])
