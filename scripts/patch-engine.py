# Applies nekodrop's changes to the staged copy of winawdl's AirDrop engine.
# Usage: python patch-engine.py <staged airdrop folder>
#
# - A stable mDNS instance name (NEKODROP_SERVICE_ID, kept by the client), so
#   an engine restart (a new awdl0 after the radio restarts) shows up on the
#   iPhone as the same device instead of a second one.
# - Spare receiver ports: when 8771 is held by another program the receiver
#   takes the next free one of 8772-8779 (advertised in the mDNS SRV record,
#   so the iPhone follows), instead of waiting for 8771 forever.
# - Sender presents no TLS client certificate.  An iPhone rejects a self-signed
#   one with SSLV3_ALERT_BAD_CERTIFICATE on both /Discover and /Ask; AirDrop
#   receivers accept connections without a client cert, and identity rides in
#   the plist record instead.  Without this the send direction cannot work.
import pathlib
import sys

NL = chr(10)
HELPER = NL.join([
    'SPARE_PORTS = range(8771, 8780)  # nekodrop: the firewall rule allows these',
    '',
    '',
    'def _bind_spare(settings, watcher):',
    '    ports = [settings.port] + [p for p in SPARE_PORTS if p != settings.port]',
    '    for port in ports:',
    '        try:',
    '            return receiver.Receiver(port, settings, watcher)',
    '        except OSError as error:',
    '            if error.errno != errno.EADDRINUSE or port == ports[-1]:',
    '                raise',
    "            logging.warning('port %d held, trying the next', port)",
    '',
    '',
    '',
])

engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
changed = False

old = 'service_id = secrets.token_hex(6)'
new = "service_id = os.environ.get('NEKODROP_SERVICE_ID') or secrets.token_hex(6)"
if new not in text:
    if old not in text:
        sys.exit('engine.py: the service id line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    if NL + 'import os' + NL not in text:
        text = text.replace(NL + 'import secrets' + NL, NL + 'import os' + NL + 'import secrets' + NL, 1)
    changed = True

old = 'server = receiver.Receiver(self.settings.port, self.settings, self.watcher)'
new = 'server = _bind_spare(self.settings, self.watcher)'
if new not in text:
    if old not in text:
        sys.exit('engine.py: the receiver bind line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    text = text.replace(NL + 'class ', NL + HELPER + 'class ', 1)
    changed = True

if changed:
    engine.write_text(text, encoding='utf-8')
print('engine patched')

# sender.py: drop the client certificate from the TLS context.  The receiver
# rejects our self-signed cert; a sender without an Apple-issued identity is
# expected to present none at all.
sender = pathlib.Path(sys.argv[1]) / 'sender.py'
text = sender.read_text(encoding='utf-8')
old = NL.join([
    '    certificate, key = identity.ensure_self_signed(keys_dir)',
    '    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)',
    '    # A receiver visible to everyone presents a certificate nothing vouches',
    '    # for, and takes ours the same way.',
    '    context.check_hostname = False',
    '    context.verify_mode = ssl.CERT_NONE',
    '    context.load_cert_chain(certificate, key)',
])
new = NL.join([
    '    # Present no client certificate.  An iPhone in Everyone mode rejects a',
    '    # self-signed one with SSLV3_ALERT_BAD_CERTIFICATE on every connection.',
    '    # AirDrop receivers accept a connection with no client cert; identity',
    '    # is carried in the plist record, not in the TLS client certificate.',
    '    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)',
    '    context.check_hostname = False',
    '    context.verify_mode = ssl.CERT_NONE',
])
if new not in text:
    if old not in text:
        sys.exit('sender.py: the client_context body moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    sender.write_text(text, encoding='utf-8')
    print('sender patched')

# engine.py: follow a receiver that announces itself again.  After a transfer
# in either direction an iPhone withdraws its AirDrop service and comes back
# under a new instance (#4: peer_gone, then WinError 10061 on its old port,
# then every retry failed at once with "gone").  A send to a peer that is gone,
# or refused on its old port, can wait briefly for that same peer to reappear.
# The peer key is the address. Different devices can advertise the same name.
FOLLOW = [
    (
        '        self._shy_said = 0' + NL,
        '        self._shy_said = 0' + NL +
        '        self._gone_names = {}  # last name at a previously selectable address' + NL,
    ),
    (
        NL.join([
            '            for key in [key for key in self._peers if key not in located]:',
            '                if self._peers.pop(key).name:',
            '                    gone.append(key)',
        ]),
        NL.join([
            '            for key in [key for key in self._peers if key not in located]:',
            '                old = self._peers.pop(key)',
            '                if old.name:',
            '                    self._gone_names[key] = old.name',
            '                    gone.append(key)',
        ]),
    ),
    (
        '    def link(self, scope):' + NL,
        NL.join([
            '    def find(self, key, name, wait, cancel, *, old_port=None):',
            '        """Wait for the same selected peer to advertise an eligible endpoint."""',
            '        deadline = time.monotonic() + wait',
            '        while not cancel.is_set():',
            '            peer = self.peer(key)',
            '            if peer and peer.name == name and peer.port != old_port:',
            '                return peer',
            '            remaining = deadline - time.monotonic()',
            '            if remaining <= 0 or not self.wanted.is_set():',
            '                return None',
            '            cancel.wait(min(0.5, remaining))',
            '        return None',
            '',
            '    def previous_name(self, key):',
            '        with self._lock:',
            "            return self._gone_names.get(key, '')",
            '',
            '    def link(self, scope):',
            '',
        ]),
    ),
    (
        NL.join([
            '        peer = self.looking.peer(peer_key)',
            '        if not peer or scope is None:',
            "            failed('gone')",
            '            return',
        ]),
        NL.join([
            '        peer = self.looking.peer(peer_key)',
            '        # None for a receiver that went away: looked for again in _run.',
            '        peer_name = peer.name if peer else self.looking.previous_name(peer_key)',
            '        if not peer_name or scope is None:',
            "            failed('gone')",
            '            return',
        ]),
    ),
    (
        NL.join([
            '        watch = SendWatch(self.events, key, sender.archive_bytes(files))',
            '        with self._lock:',
        ]),
        NL.join([
            '        watch = SendWatch(self.events, key, sender.archive_bytes(files))',
            '        watch.peer_key = peer_key',
            '        watch.peer_name = peer_name',
            '        with self._lock:',
        ]),
    ),
    (
        NL.join([
            "        watch.state('asking')",
            '        try:',
            '            outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                     self.settings.name, files, watch)',
        ]),
        NL.join([
            "        watch.state('asking')",
            '        try:',
            '            if watch.cancelled:',
            '                raise InterruptedError()',
            '            if peer is None:',
            '                peer = self.looking.find(watch.peer_key, watch.peer_name, FOLLOW_SECONDS, watch.cancel_requested)',
            '                if watch.cancelled:',
            '                    raise InterruptedError()',
            '                if peer is None:',
            "                    watch.state('failed', reason='gone')",
            '                    return',
            '            try:',
            '                outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                         self.settings.name, files, watch)',
            '            except (ConnectionRefusedError, TimeoutError):',
            '                # Refused or silent before answering: the receiver may be',
            '                # announcing itself again on another port.',
            '                if watch.reached or watch.cancelled:',
            '                    raise',
            '                fresh = self.looking.find(watch.peer_key, watch.peer_name, FOLLOW_SECONDS,',
            '                                          watch.cancel_requested, old_port=peer.port)',
            '                if watch.cancelled:',
            '                    raise InterruptedError()',
            '                if fresh is None:',
            '                    raise',
            "                logging.info('receiver came back on port %d; sending again', fresh.port)",
            '                peer = fresh',
            '                outcome = sender.deliver(peer.address, peer.port, scope, self.context,',
            '                                         self.settings.name, files, watch)',
        ]),
    ),
    (
        'BIND_RETRY_SECONDS = 5' + NL,
        'BIND_RETRY_SECONDS = 5' + NL +
        'FOLLOW_SECONDS = 15  # wait for the selected receiver to announce itself again' + NL,
    ),
]
engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
changed = False
for old, new in FOLLOW:
    if new in text:
        continue
    if text.count(old) != 1:
        sys.exit('engine.py: a line the receiver-follow patch needs moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True
if changed:
    engine.write_text(text, encoding='utf-8')
    print('engine: follows receivers that come back')

# transfer.py: throughput improvements for all cards (Intel, Realtek, ...):
# - 64 KiB DVZip blocks (4x fewer block headers / TLS records, matches AirDrop spec).
# - Adaptive incompressible-stream bypass: after 4 consecutive blocks do not shrink
#   under zlib level 1 (photos, HEIC, MP4, MOV, ZIP), emit DVZIP_STORED directly for
#   the next 32 blocks (2 MiB) before re-probing one block, cutting sender CPU ~90%
#   on media while still compressing text/mixed archives.
# - 0.5 s end-marker wait (down from 2 s) for iPhone videos/Live Photos that omit the
#   final chunked 0-length terminator.
transfer_path = pathlib.Path(sys.argv[1]) / 'transfer.py'
text = transfer_path.read_text(encoding='utf-8')
TRANSFER_PATCHES = [
    ('END_MARKER_WAIT_SECONDS = 2' + NL, 'END_MARKER_WAIT_SECONDS = 0.5  # nekodrop: faster video/Live Photo completion' + NL),
    ('DVZIP_BLOCK_BYTES = 16 * KIB' + NL, 'DVZIP_BLOCK_BYTES = 64 * KIB  # nekodrop: 64 KiB DVZip blocks' + NL),
    (
        NL.join([
            'def encode_dvzip(pieces):',
            '    """Yield the stream in `pieces` as DVZip blocks, each compressed only if',
            '    that makes it smaller."""',
            '    held = bytearray()',
            '    for piece in pieces:',
            '        held += piece',
            '        while len(held) >= DVZIP_BLOCK_BYTES:',
            '            yield _dvzip_block(bytes(held[:DVZIP_BLOCK_BYTES]))',
            '            del held[:DVZIP_BLOCK_BYTES]',
            '    if held:',
            '        yield _dvzip_block(bytes(held))',
        ]),
        NL.join([
            'def _dvzip_stored(data):',
            '    return (len(data) | DVZIP_STORED).to_bytes(4, \'big\') + data',
            '',
            '',
            'def encode_dvzip(pieces):',
            '    """Yield the stream in `pieces` as DVZip blocks, each compressed only if',
            '    that makes it smaller. Skips zlib on sustained incompressible runs (e.g.',
            '    photos/videos) and re-probes periodically so mixed archives still shrink."""',
            '    held = bytearray()',
            '    pos = 0',
            '    incomp_streak = 0',
            '    skip_left = 0',
            '    for piece in pieces:',
            '        held += piece',
            '        while len(held) - pos >= DVZIP_BLOCK_BYTES:',
            '            block = bytes(memoryview(held)[pos:pos + DVZIP_BLOCK_BYTES])',
            '            pos += DVZIP_BLOCK_BYTES',
            '            if skip_left > 0:',
            '                skip_left -= 1',
            '                yield _dvzip_stored(block)',
            '            else:',
            '                out = _dvzip_block(block)',
            '                if int.from_bytes(out[:4], \'big\') & DVZIP_STORED:',
            '                    incomp_streak += 1',
            '                    if incomp_streak >= 4:',
            '                        skip_left = 32',
            '                else:',
            '                    incomp_streak = 0',
            '                yield out',
            '        if pos >= 4 * DVZIP_BLOCK_BYTES:',
            '            del held[:pos]',
            '            pos = 0',
            '    if pos < len(held):',
            '        tail = bytes(memoryview(held)[pos:])',
            '        yield _dvzip_stored(tail) if skip_left > 0 else _dvzip_block(tail)',
        ]),
    ),
]
changed = False
for old, new in TRANSFER_PATCHES:
    if new in text:
        continue
    if text.count(old) != 1:
        sys.exit(f'transfer.py: expected line moved ({old[:48]!r}); update patch-engine.py')
    text = text.replace(old, new, 1)
    changed = True
if changed:
    transfer_path.write_text(text, encoding='utf-8')
    print('transfer: 64 KiB DVZip + adaptive stored bypass + 0.5 s tail wait')

# sender.py & receiver.py: 2 MiB socket buffers and TCP_NODELAY so bursty AWDL
# presence slots do not stall behind Winsock's 64 KiB default TCP window.
sender_path = pathlib.Path(sys.argv[1]) / 'sender.py'
text = sender_path.read_text(encoding='utf-8')
old_s = NL.join([
    '        try:',
    '            sock.settimeout(self.timeout)',
    '            sock.connect((self.host, self.port, 0, self.scope))',
])
new_s = NL.join([
    '        try:',
    '            sock.settimeout(self.timeout)',
    '            for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1),',
    '                        (socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024),',
    '                        (socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)):',
    '                try:',
    '                    sock.setsockopt(*opt)',
    '                except OSError:',
    '                    pass',
    '            sock.connect((self.host, self.port, 0, self.scope))',
])
if new_s not in text:
    if text.count(old_s) != 1:
        sys.exit('sender.py: Connection.connect moved; update patch-engine.py')
    text = text.replace(old_s, new_s, 1)
    sender_path.write_text(text, encoding='utf-8')
    print('sender: 2 MiB socket buffers + TCP_NODELAY')

receiver_path = pathlib.Path(sys.argv[1]) / 'receiver.py'
text = receiver_path.read_text(encoding='utf-8')
old_r = NL.join([
    '    def setup(self):',
    '        self.request.settimeout(transfer.READ_IDLE_TIMEOUT_SECONDS)',
])
new_r = NL.join([
    '    def setup(self):',
    '        self.request.settimeout(transfer.READ_IDLE_TIMEOUT_SECONDS)',
    '        for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1),',
    '                    (socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024),',
    '                    (socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024)):',
    '            try:',
    '                self.request.setsockopt(*opt)',
    '            except OSError:',
    '                pass',
])
if new_r not in text:
    if text.count(old_r) != 1:
        sys.exit('receiver.py: Handler.setup moved; update patch-engine.py')
    text = text.replace(old_r, new_r, 1)
    receiver_path.write_text(text, encoding='utf-8')
    print('receiver: 2 MiB socket buffers + TCP_NODELAY')

# Airdraft keeps one sender/receiver process. A paused tray can still discover
# peers and send, while the same engine withdraws its receive advertisement.
def replace_once(source, old, new, name):
    if new in source:
        return source
    if source.count(old) != 1:
        sys.exit(f'{name}: expected upstream code moved')
    return source.replace(old, new, 1)


text = engine.read_text(encoding='utf-8')
for old, new in (
    ('        self.scope = None\n        self._lock = threading.Lock()',
     '        self.scope = None\n        self.listen = not args.send_only\n        self._lock = threading.Lock()'),
    ('            if found and not self.receiving.server and time.monotonic() >= next_bind:\n'
     '                next_bind = time.monotonic() + BIND_RETRY_SECONDS\n'
     '                self.receiving.start(*found)',
     '            with self._lock:\n'
     '                if self.listen and found and not self.receiving.server and time.monotonic() >= next_bind:\n'
     '                    next_bind = time.monotonic() + BIND_RETRY_SECONDS\n'
     '                    self.receiving.start(*found)'),
    ("        elif do == 'send':",
     "        elif do == 'receive':\n"
     "            with self._lock:\n"
     "                self.listen = bool(command.get('on'))\n"
     "                if not self.listen:\n"
     "                    self.receiving.stop()\n"
     "        elif do == 'send':"),
    ("    parser.add_argument('--loopback',",
     "    parser.add_argument('--send-only', action='store_true')\n"
     "    parser.add_argument('--service-id')\n"
     "    parser.add_argument('--external-beacon', action='store_true')\n"
     "    parser.add_argument('--loopback',"),
    ("                        default=receiver.valid_dir('~/Downloads'))",
     "                        default=None)"),
    ("    return parser.parse_args(argv)",
     "    args = parser.parse_args(argv)\n"
     "    if args.dest is None:\n"
     "        try:\n"
     "            args.dest = receiver.valid_dir('~/Downloads')\n"
     "        except argparse.ArgumentTypeError as error:\n"
     "            parser.error(str(error))\n"
     "    return args"),
    ('    args = parse_args(argv)\n', '    args = parse_args(argv)\n'
     "    if args.service_id:\n        os.environ['NEKODROP_SERVICE_ID'] = args.service_id\n"),
    ('            files = sender.outgoing(paths)',
     '            files = sender.outgoing(paths, rename_duplicates=True)'),
    ('                self.announcer = mdns.Announcer(',
     '                announcer = mdns.Announcer('),
    ('                self.announcer.start()',
     '                announcer.start()\n                self.announcer = announcer'),
):
    text = replace_once(text, old, new, 'Airdraft receive preference')
text = replace_once(text, '''        self.cancelled = False
        self.was_accepted = False''', '''        self.cancel_requested = threading.Event()
        self.was_accepted = False''', 'Airdraft transfer cancellation')
text = replace_once(text, '''    def cancel(self):
        self.cancelled = True
        connection = self._connection''', '''    @property
    def cancelled(self):
        return self.cancel_requested.is_set()

    def cancel(self):
        self.cancel_requested.set()
        connection = self._connection''', 'Airdraft transfer cancellation')
text = replace_once(text, '''        except TimeoutError:
            watch.state('failed', reason='stalled' if watch.was_accepted else 'no_answer')
        except (OSError, http.client.HTTPException, ValueError) as error:
            if watch.cancelled:
                watch.state('cancelled')
            else:''', '''        except (OSError, http.client.HTTPException, ValueError) as error:
            if watch.cancelled:
                watch.state('cancelled')
            elif isinstance(error, TimeoutError):
                watch.state('failed', reason='stalled' if watch.was_accepted else 'no_answer')
            else:''', 'Airdraft transfer timeout reporting')
engine.write_text(text, encoding='utf-8')

# The native tray owns its Bluetooth advertisement. Keep the existing beacon
# for standalone engine callers and confirm the first query before signaling
# that the tray should start its publisher.
text = replace_once(text, '''    def __init__(self, events, context, fixed):
        self.events = events
        self.context = context
        self.fixed = fixed''', '''    def __init__(self, events, context, fixed, *, external_beacon=False):
        self.events = events
        self.context = context
        self.external_beacon = external_beacon
        self.fixed = fixed''', 'Airdraft discovery publisher')
text = replace_once(text, '''            browser = mdns.Browser(receiver.SERVICE_TYPE, scope)
        except OSError as error:''', '''            browser = mdns.Browser(receiver.SERVICE_TYPE, scope)
            if self.external_beacon:
                try:
                    browser.ask(required=True, within=PEER_SILENCE_SECONDS)
                except OSError:
                    browser.close()
                    raise
        except OSError as error:''', 'Airdraft first discovery query')
text = replace_once(text, '''                    browser.poll()''',
                    '''                    browser.poll(within=PEER_SILENCE_SECONDS)''',
                    'Airdraft discovery query freshness')
text = replace_once(text, '''            with sender.Beacon() as beacon:
                self.events.say('looking', on=True, beacon=beacon.on, why=beacon.said)''', '''            with (contextlib.nullcontext() if self.external_beacon else sender.Beacon()) as beacon:
                self.events.say('looking', on=True, beacon=bool(beacon and beacon.on),
                                why=beacon.said if beacon else '')''', 'Airdraft discovery publisher')
text = replace_once(text, '''                               dict(args.peer) if args.loopback else None)''',
                    '''                               dict(args.peer) if args.loopback else None,
                               external_beacon=args.external_beacon)''', 'Airdraft discovery publisher')

# A service advertisement is enough to show a device in the scan window.
# Sending still needs a current endpoint that answered /Discover.
text = replace_once(text, '''PROBE_PAUSE_SECONDS = 2''',
                    '''PROBE_PAUSE_SECONDS = 1
DISCOVERY_TIMEOUT_SECONDS = 5''', 'Airdraft discovery retry')
text = replace_once(text, '''        self.probing = False
        self.next_probe = 0.0''',
                    '''        self.probing = False
        self.discovery_state = 'checking'
        self.next_probe = 0.0''', 'Airdraft discovery candidate')
text = replace_once(text, '''            named = [key for key, peer in self._peers.items() if peer.name]
            self._peers.clear()
        for key in named:''',
                    '''            discovered = list(self._peers)
            self._peers.clear()
        for key in discovered:''', 'Airdraft discovery stop')
text = replace_once(text, '''                if old.name:
                    self._gone_names[key] = old.name
                    gone.append(key)
            for key, (address, port) in located.items():
                peer = self._peers.setdefault(key, Peer(address, port))
                peer.address, peer.port = address, port
                if not peer.name and not peer.probing and now >= peer.next_probe:''',
                    '''                if old.name:
                    self._gone_names[key] = old.name
                gone.append(key)
            for key, (address, port) in located.items():
                peer = self._peers.get(key)
                if peer is None or (peer.address, peer.port) != (address, port):
                    if peer is not None and peer.name:
                        self._gone_names[key] = peer.name
                    peer = self._peers[key] = Peer(address, port)
                    self.events.say('peer_candidate', id=key, state=peer.discovery_state)
                if not peer.name and not peer.probing and now >= peer.next_probe:''',
                    'Airdraft discovery endpoint')
text = replace_once(text, '''            found = sender.receiver_identity(peer.address, peer.port, scope, self.context)''',
                    '''            found = sender.receiver_identity(peer.address, peer.port, scope, self.context,
                                             timeout=DISCOVERY_TIMEOUT_SECONDS)''', 'Airdraft discovery timeout')
text = replace_once(text, '''        with self._lock:
            peer.probing = False
            # None is a receiver that keeps its name for those it knows, or
            # would not have our certificate at all.
            peer.shy = found is None
            peer.next_probe = time.monotonic() + (
                UNWILLING_PAUSE_SECONDS if peer.shy else PROBE_PAUSE_SECONDS)
            still_there = self._peers.get(key) is peer and not stop.is_set()
            if found and still_there:
                peer.name, peer.model = found
        if found and still_there:
            self.events.say('peer', id=key, name=peer.name, model=peer.model)''',
                    '''        with self._lock:
            if self._peers.get(key) is not peer or stop.is_set():
                return
            peer.probing = False
            peer.shy = found is None
            peer.next_probe = time.monotonic() + (
                UNWILLING_PAUSE_SECONDS if peer.shy else PROBE_PAUSE_SECONDS)
            if found:
                peer.name, peer.model = found
                self.events.say('peer', id=key, name=peer.name, model=peer.model)
            else:
                state = 'restricted' if peer.shy else 'retrying'
                if peer.discovery_state != state:
                    peer.discovery_state = state
                    self.events.say('peer_candidate', id=key, state=state)''',
                    'Airdraft discovery result')
engine.write_text(text, encoding='utf-8')

# A bound socket can still fail to send on AWDL. Require one successful
# announcement before reporting receive readiness to the tray.
mdns_path = engine.with_name('mdns.py')
text = mdns_path.read_text(encoding='utf-8')
text = replace_once(text, 'MAX_PACKET = 9000', '''MAX_PACKET = 9000
# Fit each query in the IPv6 minimum MTU, excluding IPv6 and UDP headers.
MAX_QUERY_PAYLOAD = 1232''', 'Airdraft discovery query size')
text = replace_once(text, '''def _record_value(packet, rtype, start, end):''', '''def query_packets(questions):
    batch = []
    size = HEADER_BYTES
    for name, rtype in questions:
        question_size = len(encode_name(name)) + 4
        if batch and size + question_size > MAX_QUERY_PAYLOAD:
            yield query(batch)
            batch.clear()
            size = HEADER_BYTES
        batch.append((name, rtype))
        size += question_size
    if batch:
        yield query(batch)


def _record_value(packet, rtype, start, end):''', 'Airdraft discovery query batches')
text = replace_once(text, '''    def questions(self):
        """What is still worth asking: the type, and each instance that has
        been named without saying where it is."""
        missing = sorted(self._names - self._services.keys())
        return [(self.type_name, TYPE_PTR)] + [(name, TYPE_SRV) for name in missing]''',
                    '''    def questions(self, within=None):
        """Ask for the service type and recently named instances without an endpoint."""
        oldest = time.monotonic() - within if within is not None else None
        missing = sorted(name for name in self._names - self._services.keys()
                         if oldest is None or self._heard.get(name, 0) >= oldest)
        return [(self.type_name, TYPE_PTR)] + [(name, TYPE_SRV) for name in missing]''',
                    'Airdraft discovery query freshness')
text = replace_once(text, '''                else:
                    self._names.discard(value)
            elif rtype == TYPE_SRV and name.endswith('.' + self.type_name):
                # A responder that leaves its address out is still at the
                # address its answer came from.
                self._services[name] = (*value, source)
            elif rtype == TYPE_AAAA and value.startswith('fe80:'):
                self._addresses[name] = value''',
                    '''                else:
                    self._forget(value)
            elif rtype == TYPE_SRV and name.endswith('.' + self.type_name):
                # An SRV identifies the AirDrop instance even when the PTR
                # arrives separately or only the SRV is refreshed.
                if ttl and value[1]:
                    self._services[name] = (*value, source)
                    self._names.add(name)
                    self._heard[name] = time.monotonic()
                else:
                    self._forget(name)
            elif rtype == TYPE_AAAA and value.startswith('fe80:'):
                if ttl:
                    self._addresses[name] = value
                elif self._addresses.get(name) == value:
                    self._addresses.pop(name)

    def _forget(self, name):
        self._names.discard(name)
        self._services.pop(name, None)
        self._heard.pop(name, None)''', 'Airdraft AirDrop service records')
text = replace_once(text, '''    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping.set()
        self.thread.join()
        self._send(self.service.response(alive=False))''', '''    def start(self):
        try:
            self._send(self.service.response(), required=True)
            self.thread.start()
        except (OSError, RuntimeError):
            self.sock.close()
            raise

    def stop(self):
        self.stopping.set()
        self.thread.join()
        self._send(self.service.response(alive=False))''', 'Airdraft first announcement')
text = replace_once(text, '''    def _send(self, packet):
        try:
            self.sock.sendto(packet, (GROUP, PORT, 0, self.interface_index))
        except OSError as error:
            logging.warning('mdns: send failed: %s', error)''', '''    def _send(self, packet, *, required=False):
        try:
            self.sock.sendto(packet, (GROUP, PORT, 0, self.interface_index))
        except OSError as error:
            if required:
                raise
            logging.warning('mdns: send failed: %s', error)''', 'Airdraft announcement error')
text = replace_once(text, '''        announcement = self.service.response()
        last_sent = 0.0''', '''        announcement = self.service.response()
        last_sent = time.monotonic()''', 'Airdraft announcement interval')
text = replace_once(text, '''    def poll(self):
        """Ask if it is time to, wait for one packet, and return what can be
        reached so far."""
        if time.monotonic() - self._last_asked >= QUERY_PERIOD_S:
            self._last_asked = time.monotonic()
            try:
                self.sock.sendto(query(self.instances.questions()),
                                 (GROUP, PORT, 0, self.interface_index))
            except OSError as error:
                logging.warning('mdns: query failed: %s', error)''', '''    def ask(self, *, required=False, within=None):
        self._last_asked = time.monotonic()
        try:
            for packet in query_packets(self.instances.questions(within)):
                self.sock.sendto(packet, (GROUP, PORT, 0, self.interface_index))
        except OSError as error:
            if required:
                raise
            logging.warning('mdns: query failed: %s', error)

    def poll(self, *, within=None):
        """Ask if it is time to, wait for one packet, and return what can be
        reached so far."""
        if time.monotonic() - self._last_asked >= QUERY_PERIOD_S:
            self.ask(within=within)''', 'Airdraft first discovery query')
mdns_path.write_text(text, encoding='utf-8')

# Keep every selected path in the transfer. Outgoing.name is used in both
# the consent request and the archive, while Outgoing.path stays on disk.
text = sender_path.read_text(encoding='utf-8')
text = replace_once(text, 'def receiver_identity(address, port, scope, context):',
                    'def receiver_identity(address, port, scope, context, *, timeout=CONNECT_SECONDS):',
                    'Airdraft identity probe timeout')
text = replace_once(text, '    connection = Connection(address, port, scope, context, CONNECT_SECONDS)',
                    '    connection = Connection(address, port, scope, context, timeout)',
                    'Airdraft identity probe timeout')
text = replace_once(text, '''    connection = Connection(address, port, scope, context, ASK_SECONDS)
    try:
        transfer_id = str(uuid.uuid4()).upper()
        connection.connect()
        watch.connected(connection)''', '''    connection = Connection(address, port, scope, context, CONNECT_SECONDS)
    try:
        transfer_id = str(uuid.uuid4()).upper()
        connection.connect()
        watch.connected(connection)
        connection.sock.settimeout(ASK_SECONDS)''', 'Airdraft consent connection timeout')
text = replace_once(text, 'import uuid\n', 'import unicodedata\nimport uuid\n', 'Airdraft sender names')
text = replace_once(text,
    'def outgoing(paths):\n    """The files to send, refusing what an archive cannot hold apart."""',
    'def outgoing(paths, *, rename_duplicates=False):\n'
    '    """The files to send, optionally assigning unique archive names."""',
    'Airdraft sender names')
text = replace_once(text, '''    names = [file.name.lower() for file in files]
    if len(set(names)) != len(names):
        raise Unsendable('same_name', 'two of the files have the same name')
    return files''', '''    if not rename_duplicates:
        names = [file.name.lower() for file in files]
        if len(set(names)) != len(names):
            raise Unsendable('same_name', 'two of the files have the same name')
        return files

    def archive_key(name):
        return unicodedata.normalize('NFC', name).casefold()

    reserved = {archive_key(file.name) for file in files}
    seen = set()
    for index, file in enumerate(files):
        key = archive_key(file.name)
        if key not in seen:
            seen.add(key)
            continue
        for suffix in range(1, transfer.MAX_NAME_ATTEMPTS):
            name = transfer.numbered(file.name, suffix)
            key = archive_key(name)
            if key not in reserved:
                reserved.add(key)
                files[index] = file._replace(name=name)
                break
        else:
            raise Unsendable('same_name', f'could not assign a unique name to {file.name}')
    return files''', 'Airdraft sender names')
sender_path.write_text(text, encoding='utf-8')

# Keep extracted bytes in a temporary directory on the destination volume.
# A complete and validated archive can then publish each file atomically.
text = transfer_path.read_text(encoding='utf-8')
old = '''            return _extract(archive, dest,
                            ExtractionBudget(receive_budget.byte_limit), created)'''
new = '''            with tempfile.TemporaryDirectory(prefix='.airdraft-', dir=dest) as staging:
                complete = _extract(archive, staging,
                                    ExtractionBudget(receive_budget.byte_limit), [])
                return _publish(staging, dest, complete, created)'''
text = replace_once(text, old, new, 'Airdraft staged extraction')
old = 'def store_upload(raw, dest, tid, receive_budget):'
new = '''def _publish(staging, dest, complete, created):
    # Airdraft keeps a Live Photo's still and video under the same stem.
    groups = {}
    for item in complete:
        group = ('live', item.live_photo) if item.live_photo else ('file', str(item))
        groups.setdefault(group, []).append(item)
    published = []
    for items in groups.values():
        for suffix in range(MAX_NAME_ATTEMPTS):
            names = [numbered(item, suffix) for item in items]
            if any(os.path.lexists(os.path.join(dest, name)) for name in names):
                continue
            moved = []
            try:
                for item, name in zip(items, names):
                    source = os.path.join(staging, item)
                    target = os.path.join(dest, name)
                    if os.name == 'nt':
                        os.rename(source, target)  # Windows rename does not clobber.
                    else:
                        os.link(source, target)
                    moved.append((source, target, name))
                    created.append(name)
            except FileExistsError:
                for source, target, name in reversed(moved):
                    if os.name == 'nt':
                        os.rename(target, source)
                    else:
                        os.unlink(target)
                    created.remove(name)
                continue
            published.extend(Stored(name, item.live_photo) for item, name in zip(items, names))
            break
        else:
            raise FileExistsError(items[0])
    return published


def store_upload(raw, dest, tid, receive_budget):'''
text = replace_once(text, old, new, 'Airdraft publication')
transfer_path.write_text(text, encoding='utf-8')
