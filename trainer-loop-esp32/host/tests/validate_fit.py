"""Independent semantic assertions using the unmodified official Garmin SDK."""
import datetime as dt
import hashlib
import pathlib
import subprocess
import sys
from garmin_fit_sdk import Decoder, Stream

root = pathlib.Path(sys.argv[2])
subprocess.run([sys.argv[1], str(root)], check=True)
before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.fit')}
subprocess.run([sys.argv[1], str(root)], check=True)
start = dt.datetime(2023, 11, 14, 22, 13, 20, tzinfo=dt.timezone.utc)
for name, elapsed, active in [('complete', 10, 10), ('partial', 3, 3), ('paused', 8, 4), ('recovered', 7, 3)]:
    path = root / (name + '.fit')
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before[path.name]
    decoder = Decoder(Stream.from_file(str(path)))
    assert decoder.check_integrity(), name
    messages, errors = Decoder(Stream.from_file(str(path))).read()
    assert not errors, (name, errors)
    assert messages['file_id_mesgs'][0]['manufacturer'] == 'development'
    assert messages['file_id_mesgs'][0]['product'] == 0
    assert messages['file_id_mesgs'][0]['time_created'] == start
    records = messages['record_mesgs']
    assert records[0]['timestamp'] == start + dt.timedelta(seconds=1)
    assert records[0]['power'] == 0 and records[0]['speed'] == 0
    assert records[1].get('power') is None and records[1].get('speed') is None
    for kind in ('lap_mesgs', 'session_mesgs'):
        summary = messages[kind][0]
        assert summary['total_elapsed_time'] == elapsed
        assert summary['total_timer_time'] == active
        assert summary.get('avg_heart_rate') is None, (name, kind, summary)
        assert summary.get('max_heart_rate') is None
        assert summary['avg_power'] == (100 if name in ('paused', 'recovered') else 0)
    assert messages['activity_mesgs'][0]['total_timer_time'] == active
    events = messages['event_mesgs']
    if name in ('paused', 'recovered'):
        assert [(e['event_type'], (e['timestamp'] - start).total_seconds()) for e in events] == [
            ('start', 0), ('stop', 2), ('start', 6), ('stop_all', elapsed)], (name, events)
        assert records[-1]['altitude'] < records[0]['altitude']
        assert messages['session_mesgs'][0]['total_distance'] == 12
    damaged = bytearray(path.read_bytes()); damaged[-3] ^= 1
    assert not Decoder(Stream.from_byte_array(damaged)).check_integrity()
    print(name, before[path.name], 'Garmin CRC and semantic checks passed')
