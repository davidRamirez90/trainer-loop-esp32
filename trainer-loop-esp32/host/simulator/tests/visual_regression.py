"""Capture actual production LVGL fixture screens; compare explicitly reviewed PNGs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('executable', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--capture', action='store_true', help='capture candidates, without changing baselines')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
baselines = Path(__file__).parent / 'baselines'
with tempfile.TemporaryDirectory(prefix='trainer-visual-') as storage:
    p = subprocess.Popen([str(args.executable.resolve()), '--storage', storage],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    log = []
    def command(**cmd):
        p.stdin.write(json.dumps(cmd)+'\n'); p.stdin.flush()
        response = json.loads(p.stdout.readline())
        log.append({'input': cmd, 'response': response})
        assert response['ok'], response
        return response['state']

    def verify_controls(state, fixture):
        controls = state['controls']
        ids = [control['id'] for control in controls]
        assert len(ids) == len(set(ids)), (fixture, 'duplicate visible control ids', ids)
        for control in controls:
            assert control['id'] and isinstance(control['text'], str), (fixture, control)
            assert isinstance(control['enabled'], bool), (fixture, control)
            assert control['width'] > 0 and control['height'] > 0, (fixture, control)
            assert control['x'] >= 0 and control['y'] >= 0, (fixture, control)
            assert control['x'] + control['width'] <= 368, (fixture, control)
            assert control['y'] + control['height'] <= 448, (fixture, control)
    try:
        names = command(op='fixtures')['fixtures']
        manifest = {}
        for name in names:
            state = command(op='fixture', name=name)
            verify_controls(state, name)
            path = args.output / (name+'.png')
            command(op='screenshot', path=str(path.resolve()))
            png = path.read_bytes()
            assert png[:8] == b'\x89PNG\r\n\x1a\n'
            assert int.from_bytes(png[16:20], 'big') == 368
            assert int.from_bytes(png[20:24], 'big') == 448
            assert len(png) > 1000, (name, 'unexpectedly blank screenshot')
            manifest[name] = {'sha256': hashlib.sha256(png).hexdigest(), 'screen': state['screen'],
                              'controls': state['controls']}
            if not args.capture:
                expected = baselines / (name+'.png')
                assert expected.exists(), f'No reviewed baseline for {name}'
                assert png == expected.read_bytes(), f'Visual mismatch {name}; inspect {path}'

        # Exercise the LVGL event path with raw pointer coordinates, including
        # a disabled button. The simulator applies runtime commands only after
        # the complete press/release pair so both edges hit the same tree.
        command(op='setup', ssid='Visual Regression', password='synthetic-password',
                athlete_id='visual-test-athlete', api_key='synthetic-test-key')
        command(op='advance', ms=100)
        disabled_state = command(op='fixture', name='preview-unavailable')
        disabled = next(item for item in disabled_state['controls'] if item['id'] == 'primary')
        assert disabled['text'] == 'START UNAVAILABLE' and not disabled['enabled']
        before = sum(event.startswith('ui.command ') for event in disabled_state['trace'])
        command(op='pointer_click', x=disabled['x'] + disabled['width'] // 2,
                y=disabled['y'] + disabled['height'] // 2)
        after_state = command(op='state')
        after = sum(event.startswith('ui.command ') for event in after_state['trace'])
        assert after == before, 'disabled LVGL control enqueued a UI command'

        enabled_state = command(op='fixture', name='home-empty')
        enabled = next(item for item in enabled_state['controls'] if item['id'] == 'primary')
        assert enabled['text'] == 'RETRY' and enabled['enabled']
        before = sum(event.startswith('ui.command ') for event in enabled_state['trace'])
        command(op='pointer_click', x=enabled['x'] + enabled['width'] // 2,
                y=enabled['y'] + enabled['height'] // 2)
        after_state = command(op='state')
        after = sum(event.startswith('ui.command ') for event in after_state['trace'])
        assert after == before + 1, 'enabled LVGL control did not enqueue exactly one UI command'
        assert any(event.startswith('ui.command ') and 'type=14 ' in event
                   for event in after_state['trace']), 'raw Retry tap emitted the wrong UI command'
        (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        if not args.capture:
            reviewed = json.loads((baselines/'review.json').read_text())
            assert reviewed['reviewed'] is True
            expected = json.loads((baselines/'manifest.json').read_text())
            assert manifest == expected, 'Structured visible-control or state mismatch'
        print(f'{len(names)} production LVGL fixtures captured; '+('candidates ready for review' if args.capture else 'reviewed visual/state baselines match'))
    finally:
        (args.output/'commands.json').write_text(json.dumps(log, indent=2)+'\n')
        p.stdin.close()
        try: p.wait(timeout=10)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
        (args.output/'stderr.log').write_text(p.stderr.read())
