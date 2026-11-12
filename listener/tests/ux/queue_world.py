#!/usr/bin/env python3
"""Desktop mixed-author queue provenance through authenticated protocol clients."""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..', '..', 'tools')]
from drive import Drive
from privateworld import Client


def until(fn):
    end = time.time() + 25
    while time.time() < end:
        if fn():
            return
        time.sleep(0.1)
    raise AssertionError('queue condition timed out')


def main():
    ap = argparse.ArgumentParser()
    for arg in ('listener', 'url', 'token-file', 'root-token'):
        ap.add_argument('--' + arg, required=True)
    a = ap.parse_args()
    port = urlparse(a.url).port
    person = Client(port, open(a.token_file).read().strip())
    root = Client(port, open(a.root_token).read().strip())
    resolve = lambda n: root.req('resolve', {'name': n, 'op': 'read'})['result']['object']
    agent, route = resolve('root'), resolve('root.route')
    tmp = tempfile.mkdtemp(prefix='orbis-queue-')
    sock = os.path.join(tmp, 'drive.sock')
    log = open(os.path.join(tmp, 'listener.log'), 'w')
    p = subprocess.Popen([a.listener, '--url', a.url, '--token-file', a.token_file,
                          '--drive', sock, '--data-dir', os.path.join(tmp, 'data')], stderr=log)
    try:
        d = Drive(sock, connect_timeout=30)
        d.wait('attached', timeout_ms=30000)
        d.resize(1280, 800)
        until(lambda: 'principal:root' in d.targets())
        d.click('principal:root')
        until(lambda: 'input' in d.targets())
        r = person.req('send', {'to': [agent], 'route': route, 'text': 'Count slowly to forty'})
        assert r.get('ok'), r
        def busy():
            # Read the Principal, not an optional Field: before the first wake
            # loop/current may not exist, and after completion it retains the
            # last Message. loop/iteration is the kernel's active-cycle marker.
            reply = root.req('read', {'object': agent})
            assert reply.get('ok'), reply
            fields = reply['result']['object/fields']
            return (fields.get('loop/iteration', {}).get('field/value') or 0) > 0
        until(busy)
        for i in range(9):
            client = person if i % 2 == 0 else root
            r = client.req('send', {'to': [agent], 'route': route,
                                   'text': f'queue row {i}: ' + 'long mixed sender text ' * 20 + '\nsecond line'})
            assert r.get('ok'), r
        until(lambda: len(d.state().get('queue') or []) >= 9)
        d.wait('settle', frames=12, timeout_ms=15000)
        rows = d.state()['queue']
        assert len({row['from'] for row in rows}) == 2, rows
        targets = d.state()['targets']
        for row in rows[:2]:
            steer = 'button:steer:' + row['message'][:8]
            assert (steer in targets) == (row['from'] == rows[0]['from']), (row, targets)
        d.resize(390, 844)
        d.wait('settle', frames=12, timeout_ms=15000)
        d.click('tab:chat')
        d.wait('settle', frames=12, timeout_ms=15000)
        targets = d.state()['targets']
        buttons = [v for k, v in targets.items() if k.startswith('button:dequeue:')]
        assert buttons, targets
        for rect in buttons[:2]:
            assert rect['w'] >= 44 and rect['h'] >= 44, rect
        heading, input_rect = targets['queue:heading'], targets['input']
        assert heading['y'] + heading['h'] <= input_rect['y'], (heading, input_rect)
        texts = [k for k in targets if k.startswith('queue:text:')]
        assert texts, targets
        d.click(texts[0])
        d.wait('settle', frames=4, timeout_ms=15000)
        d.key('Escape')
        d.wait('settle', frames=4, timeout_ms=15000)
        targets = d.state()['targets']
        assert 'queue:heading' in targets and 'input' in targets, targets
        rows = d.state()['queue']
        assert len({row['message'] for row in rows}) == len(rows), rows
        own, other = rows[0], rows[1]
        def prefix(message):
            return message[:8]
        own_target = 'button:steer:' + prefix(own['message'])
        other_target = 'button:steer:' + prefix(other['message'])
        assert own_target in targets, targets
        assert other_target not in targets, targets
        assert 'button:up:' + prefix(other['message']) not in targets, targets
        assert 'button:down:' + prefix(other['message']) not in targets, targets
        # Check authority synchronously while the original slow wake is active;
        # later queue membership is not evidence of refusal (the lane may consume it).
        refused = person.req('send', {'to': [agent], 'route': route,
                                       'steer': True, 'message': other['message'], 'text': ''})
        assert not refused.get('ok'), refused
        assert 'not-allowed' in str(refused), refused
        print('foreign promotion response:', refused)
        original = root.req('read', {'object': own['message']})
        assert original.get('ok'), original
        d.click(own_target)
        until(lambda: own['message'] not in [r['message'] for r in d.state().get('queue', [])])
        after = root.req('read', {'object': own['message']})
        assert after.get('ok'), after
        before_fields = original['result']['object/fields']
        after_fields = after['result']['object/fields']
        for field in ('message/from', 'message/route', 'message/text'):
            assert before_fields[field]['field/value'] == after_fields[field]['field/value'], field
        # Promotion preserves the Message rather than rewriting its kind.
        assert before_fields.get('message/kind') == after_fields.get('message/kind')
        remaining = d.state().get('queue', [])
        assert len({r['message'] for r in remaining}) == len(remaining), remaining
        # Capture this Message's own protocol history, not a racy later queue assertion.
        evidence = root.req('history', {'stream': other['message'], 'limit': 50})
        assert evidence.get('ok'), evidence
        print('foreign Message history:', evidence)
        print('PASS: desktop mixed-author queue; own original promoted by id; foreign steer absent; provenance unchanged')
    finally:
        p.terminate()
        p.wait(timeout=15)
        person.close()
        root.close()
        log.close()


if __name__ == '__main__':
    main()
