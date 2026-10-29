#!/usr/bin/env python3
"""Inspect/apply/restore settings on an explicitly selected dedicated camera NIC.

No package installs, route/proxy changes or persistent configuration writes.
Stop camera acquisition before apply/restore: RX ring changes can reset a link.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def snapshot(interface):
    root = Path('/sys/class/net') / interface
    if not root.is_dir():
        raise RuntimeError('Network interface not found: ' + interface)
    ring = run('ethtool', '-g', interface)
    maximum, current = ring.split('Current hardware settings:', 1)

    def rx(text):
        match = re.search(r'^RX:\s+(\d+)', text, re.M)
        if not match:
            raise RuntimeError('Cannot read this NIC RX ring capacity')
        return int(match.group(1))

    return dict(interface=interface, mac=(root / 'address').read_text().strip(),
                mtu=int((root / 'mtu').read_text()), rx=rx(current), rx_max=rx(maximum),
                rmem_max=int(Path('/proc/sys/net/core/rmem_max').read_text()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['status', 'apply', 'restore'])
    parser.add_argument('--interface', required=True)
    parser.add_argument('--state', type=Path, help='Explicit backup JSON path; never uploaded')
    parser.add_argument('--mtu', type=int, default=9000)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.:-]+', args.interface):
        parser.error('Invalid interface name')
    current = snapshot(args.interface)
    if args.action == 'status':
        print(json.dumps(current, indent=2))
        return
    if os.geteuid() != 0:
        parser.error('Apply/restore requires host administrator privileges')
    if args.state is None:
        parser.error('Apply/restore requires --state /path/to/backup.json')
    if run('ip', 'route', 'show', 'default', 'dev', args.interface):
        raise RuntimeError('Selected NIC carries a default route; choose a dedicated camera NIC')
    if args.action == 'apply':
        if not 1500 <= args.mtu <= 9000:
            parser.error('MTU must be between 1500 and 9000, supported by the entire camera link')
        if args.state.exists():
            raise RuntimeError('Backup already exists; restore first or select a new path')
        args.state.parent.mkdir(parents=True, exist_ok=True)
        with args.state.open('x') as stream:
            json.dump(current, stream, indent=2)
        target = dict(current, mtu=args.mtu, rx=current['rx_max'],
                      rmem_max=max(current['rmem_max'], 134217728))
    else:
        target = json.loads(args.state.read_text())
        if target['interface'] != args.interface or target['mac'] != current['mac']:
            raise RuntimeError('Saved NIC identity differs; refusing to restore another device')
    # Backup exists before the first mutation so a partial failure is recoverable.
    if current['mtu'] != target['mtu']:
        run('ip', 'link', 'set', 'dev', args.interface, 'mtu', str(target['mtu']))
    if current['rx'] != target['rx']:
        run('ethtool', '-G', args.interface, 'rx', str(target['rx']))
    if current['rmem_max'] != target['rmem_max']:
        run('sysctl', '-w', 'net.core.rmem_max=' + str(target['rmem_max']))
    result = snapshot(args.interface)
    if any(result[key] != target[key] for key in ('mtu', 'rx', 'rmem_max')):
        raise RuntimeError('Readback differs from request; backup remains available for restore')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
