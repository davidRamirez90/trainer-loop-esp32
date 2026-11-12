"""Validate generated simulator files with Garmin's official FIT SDK."""

from __future__ import annotations

import pathlib
import sys

from garmin_fit_sdk import Decoder, Stream

for filename in sys.argv[1:]:
    path = pathlib.Path(filename)
    decoder = Decoder(Stream.from_file(str(path)))
    assert decoder.check_integrity(), f"FIT CRC/integrity failed: {path}"
    messages, errors = Decoder(Stream.from_file(str(path))).read()
    assert not errors, f"FIT decode failed for {path}: {errors}"
    records = messages.get("record_mesgs", [])
    sessions = messages.get("session_mesgs", [])
    assert records, f"FIT has no telemetry records: {path}"
    assert sessions, f"FIT has no session summary: {path}"
    timestamps = [record["timestamp"] for record in records if "timestamp" in record]
    assert timestamps == sorted(timestamps), f"FIT record timestamps regress: {path}"
    session = sessions[0]
    elapsed = session.get("total_elapsed_time", 0)
    active = session.get("total_timer_time", 0)
    assert elapsed >= active > 0, f"FIT session timing is invalid: {path}"
    assert session.get("sport") == "cycling", f"FIT activity is not cycling: {path}"
    print(f"Garmin FIT SDK validated {path.name}: {len(records)} records, {elapsed:.1f}s")

