# Copyright (c) 2026 Airdraft
# SPDX-License-Identifier: GPL-2.0-only
"""Check the staged AirDrop engines and native identity over IPv6 loopback."""
import hashlib
import io
import json
import os
import pathlib
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from unittest.mock import Mock, patch


@contextmanager
def running_engine(stage, keys, output, *options):
    args = [stage / 'python' / 'python.exe', '-u', stage / 'airdrop' / 'engine.py',
            '--outdir', output, '--keys', keys, '--loopback', '--port', '0', *options]
    with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, encoding='utf-8') as engine:
        lines = queue.Queue()

        def read():
            try:
                for line in engine.stdout:
                    lines.put(json.loads(line))
            finally:
                lines.put(None)

        def event(kind, *, timeout=8, **fields):
            deadline = time.monotonic() + timeout
            while True:
                try:
                    message = lines.get(timeout=max(0, deadline - time.monotonic()))
                except queue.Empty:
                    raise AssertionError(f'Timed out waiting for {kind} {fields}') from None
                if message is None or message['event'] in ('fatal', 'error'):
                    raise AssertionError(f'Expected {kind} {fields}, got {message}')
                if message['event'] == kind and fields.items() <= message.items():
                    return message
                if time.monotonic() >= deadline:
                    raise AssertionError(f'Timed out waiting for {kind} {fields}')

        def send(action, **settings):
            engine.stdin.write(json.dumps({'do': action, **settings}) + '\n')
            engine.stdin.flush()

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        try:
            event('ready')
            event('link', up=True)
            yield send, event, lines
            send('quit')
            assert engine.wait(timeout=8) == 0
        finally:
            if engine.poll() is None:
                engine.kill()
                engine.wait(timeout=8)
            reader.join(timeout=8)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def verify_transfer(stage, work, keys, output, recv, received, port):
    left = work / 'left'
    right = work / 'right'
    left.mkdir()
    right.mkdir()
    sources = [left / 'Caf\u00e9.txt', right / 'Caf\u00e9.txt', left / 'Caf\u00e9-1.txt',
               left / 'large.bin']
    for index, source in enumerate(sources[:3]):
        source.write_bytes(f'Input {index}\n'.encode())
    block = os.urandom(1024 * 1024)
    with sources[3].open('wb') as stream:
        for _ in range(100):
            stream.write(block)
    names = ['Caf\u00e9.txt', 'Caf\u00e9-2.txt', 'Caf\u00e9-1.txt', 'large.bin']
    expected = {name: sha256(source) for name, source in zip(names, sources)}

    with running_engine(stage, keys, output, '--name', 'Airdraft Sender', '--send-only',
                        '--peer', f'[::1]:{port}') as (send, sent, _):
        send('look', on=True)
        candidate = sent('peer_candidate', state='checking')
        peer = sent('peer', name='Airdraft Receiver')
        assert candidate['id'] == peer['id']
        send('send', id='test', peer=peer['id'], files=[str(source) for source in sources])
        started = sent('send', id='test')
        assert started['state'] == 'asking', started
        asked = received('asked', sender='Airdraft Sender')
        assert [(item['name'], item['size']) for item in asked['files']] == [
            (name, source.stat().st_size) for name, source in zip(names, sources)]
        recv('answer', id=asked['id'], accept=True)
        stored = received('stored', timeout=30, id=asked['id'])
        result = sent('send', id='test', timeout=30, state='sent')
        assert result['done'] == result['total'] == started['total']
        assert set(stored['names']) == set(expected), stored
        assert {file.name: sha256(file) for file in output.iterdir()} == expected


def verify_advertisement(stage, keys, output):
    sys.path.insert(0, str(stage / 'airdrop'))
    try:
        import engine
        import mdns
    finally:
        sys.path.pop(0)

    args = engine.parse_args(['--keys', str(keys), '--outdir', str(output), '--port', '0'])
    events = io.StringIO()
    receiving = engine.Engine(args, engine.Events(events)).receiving
    multicast = Mock()
    multicast.sendto.side_effect = OSError('AWDL multicast unavailable')
    multicast.recvfrom.side_effect = OSError('AWDL multicast unavailable')
    with patch.object(mdns, 'open_socket', return_value=multicast):
        try:
            assert not receiving.start(1, 'fe80::1'), 'Receiver reported ready without sending an announcement'
            assert receiving.server is None and receiving.announcer is None
        finally:
            receiving.stop()
    multicast.close.assert_called_once()
    replies = [json.loads(line) for line in events.getvalue().splitlines()]
    assert replies and replies[-1]['on'] is False and replies[-1]['reason'] == 'AWDL multicast unavailable'


def verify_discovery_updates():
    import engine
    import mdns

    instance = 'phone._airdrop._tcp.local'
    peer_id = 'fe80::1234'
    services = mdns.Instances('_airdrop._tcp.local')

    def advertise(port, ttl):
        record = mdns._record(instance, mdns.TYPE_SRV, mdns.CLASS_IN, ttl,
                              struct.pack('!HHH', 0, 0, port) + mdns.encode_name('phone.local'))
        services.feed(struct.pack('!HHHHHH', 0, mdns.RESPONSE_FLAGS, 0, 1, 0, 0) + record, peer_id)

    wire = io.StringIO()
    scan = engine.Looking(engine.Events(wire), None, None)
    stop = threading.Event()
    with patch.object(engine.threading, 'Thread'):
        advertise(8770, 60)
        assert services.devices() == {peer_id: (peer_id, 8770)}
        scan._reconcile(services.devices(), 25, stop)
        original = scan._peers[peer_id]
        assert scan.peer(peer_id) is None
        with patch.object(engine.sender, 'receiver_identity', side_effect=engine.sender.Unreachable('timeout')):
            scan._probe(peer_id, original, 25, stop)
        advertise(8772, 60)
        scan._reconcile(services.devices(), 25, stop)
        updated = scan._peers[peer_id]
        assert updated.port == 8772 and updated is not original
        with patch.object(engine.sender, 'receiver_identity', return_value=('iPhone', None)):
            scan._probe(peer_id, original, 25, stop)
            assert scan.peer(peer_id) is None
            scan._probe(peer_id, updated, 25, stop)
        assert scan.peer(peer_id) is updated
        advertise(8772, 0)
        scan._reconcile(services.devices(), 25, stop)
        assert scan.peer(peer_id) is None

    messages = [json.loads(line) for line in wire.getvalue().splitlines()]
    assert [item['event'] for item in messages] == [
        'peer_candidate', 'peer_candidate', 'peer_candidate', 'peer', 'peer_gone']
    assert [item['state'] for item in messages if item['event'] == 'peer_candidate'] == [
        'checking', 'retrying', 'checking']


def verify_crowded_discovery():
    import mdns

    service = '_airdrop._tcp.local'
    old = 'departed.' + service
    active = {f'{index:040x}.{service}' for index in range(64)}
    multicast = Mock()
    with patch.object(mdns, 'open_socket', return_value=multicast):
        browser = mdns.Browser(service, 25)

    def announce(name):
        record = mdns._record(service, mdns.TYPE_PTR, mdns.CLASS_IN, 4500, mdns.encode_name(name))
        packet = struct.pack('!HHHHHH', 0, mdns.RESPONSE_FLAGS, 0, 1, 0, 0) + record
        browser.instances.feed(packet, 'fe80::1234')

    with patch.object(mdns.time, 'monotonic', return_value=100):
        announce(old)
    with patch.object(mdns.time, 'monotonic', return_value=125):
        for name in active:
            announce(name)
        browser.ask(required=True, within=20)
    packets = [call.args[0] for call in multicast.sendto.call_args_list]
    assert len(packets) > 1
    assert all(len(packet) <= 1232 for packet in packets)
    assert set().union(*(mdns.question_names(packet) for packet in packets)) == active | {service}
    browser.close()


def verify_transfer_retry():
    import engine

    target, other = 'fe80::1', 'fe80::2'
    name = 'iPhone'
    scan = engine.Looking(None, None, None)
    scan.wanted.set()
    scan._gone_names[target] = name
    scan._peers[other] = engine.Peer(other, 8771)
    scan._peers[other].name = name
    cancel = threading.Event()
    assert scan.find(target, name, 0, cancel) is None, 'A different device had the same name'
    peer = scan._peers[target] = engine.Peer(target, 8770)
    peer.name = 'Another receiver'
    assert scan.find(target, name, 0, cancel) is None, 'The address returned with a different name'
    peer.name = name
    assert scan.find(target, name, 0, cancel, old_port=8770) is None

    wire = io.StringIO()
    events = engine.Events(wire)
    settings = Mock()
    settings.name = 'Airdraft Sender'
    sending = engine.Sending(events, settings, scan, None)

    def watch(key):
        transfer = engine.SendWatch(events, key, 0)
        transfer.peer_key, transfer.peer_name = target, name
        sending._watches[key] = transfer
        return transfer

    def moved_port(address, port, *args):
        assert address == target
        if port == 8770:
            updated = scan._peers[target] = engine.Peer(target, 8772)
            updated.name = name
            raise ConnectionRefusedError()
        assert port == 8772
        return engine.sender.SENT

    with patch.object(engine.sender, 'deliver', side_effect=moved_port) as deliver:
        sending._run(watch('retry'), peer, [], 25)
        assert deliver.call_count == 2

    del scan._peers[target]
    waiting = threading.Event()
    find = scan.find

    def waiting_for_receiver(*args, **kwargs):
        waiting.set()
        return find(*args, **kwargs)

    with patch.object(scan, 'find', side_effect=waiting_for_receiver), \
            patch.object(engine.sender, 'deliver') as deliver:
        worker = threading.Thread(target=sending._run, args=(watch('cancel'), None, [], 25), daemon=True)
        worker.start()
        assert waiting.wait(1), 'Transfer did not reach receiver rediscovery'
        sending.cancel('cancel')
        worker.join(timeout=1)
        assert not worker.is_alive(), 'Cancel did not wake receiver rediscovery'
        deliver.assert_not_called()

    results = {event['id']: event['state'] for event in map(json.loads, wire.getvalue().splitlines())}
    assert results == {'retry': 'sent', 'cancel': 'cancelled'}, results


def verify(stage):
    with tempfile.TemporaryDirectory(dir=stage.parent) as work:
        work = pathlib.Path(work)
        keys = work / 'keys'
        output = work / 'downloads'
        output.mkdir()
        subprocess.run([stage / 'Airdraft.exe', '--selftest', keys], timeout=10, check=True)
        verify_advertisement(stage, keys, output)
        verify_discovery_updates()
        verify_crowded_discovery()
        verify_transfer_retry()
        with running_engine(stage, keys, output, '--name', 'Airdraft Receiver',
                            '--send-only', '--service-id', '123456789abc') as (recv, received, lines):
            try:
                unexpected = lines.get(timeout=1.5)
            except queue.Empty:
                pass
            else:
                raise AssertionError(f'Unexpected send-only event: {unexpected}')
            recv('receive', on=True)
            received('receiving', on=True)
            recv('receive', on=False)
            received('receiving', on=False)
            recv('receive', on=True)
            port = received('receiving', on=True)['port']
            verify_transfer(stage, work, keys, output, recv, received, port)
    print('Advertisement failure withheld receive readiness and closed both sockets.')
    print('AirDrop advertisements appeared immediately, recovered a changed port, and removed expired candidates.')
    print('Discovery queried all 64 current receivers in IPv6-sized packets and skipped a departed receiver.')
    print('Transfer retries kept the selected receiver and cancellation stopped a pending retry.')
    print('Native identity, receive controls and a 100 MiB batch with Unicode and duplicate names passed over loopback.')


if __name__ == '__main__':
    verify(pathlib.Path(sys.argv[1]).resolve())
