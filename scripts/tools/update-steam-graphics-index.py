#!/usr/bin/env python3
"""Build an offline Steam graphics index from PCGamingWiki's MediaWiki API."""
import argparse
import datetime as dt
import email.utils
import json
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ENDPOINT = 'https://www.pcgamingwiki.com/w/api.php'
USER_AGENT = 'Brinewell/1.0 (+https://github.com/Coulin9/Brinewell)'
ANNOTATIONS = {'note', 'ref', 'refcheck', 'refurl', 'cn', 'citation needed'}
TARGETS = {'infobox game', 'infobox game/row/date', 'api'}
D3D = re.compile(r'(?<![a-z0-9.])(?:(?:dx|d3d|direct3d|directx)\s*)?([1-9]|1[0-2])(?:\.[0-9]+)?(?:ex|c)?(?![a-z0-9.])', re.I)
MONTHS = ['january', 'february', 'march', 'april', 'may', 'june', 'july', 'august', 'september', 'october', 'november', 'december']


def normalize(value):
    return ' '.join(value.replace('_', ' ').lower().split())


def mask(text):
    # An unclosed comment or non-self-closing tag masks the remainder.
    pattern = re.compile(r'<!--|<(ref|nowiki)(?!\w)', re.I)
    result, pos = [], 0
    while match := pattern.search(text, pos):
        result.append(text[pos:match.start()])
        if match.group(1) is None:
            end = text.find('-->', match.end())
            pos = len(text) if end < 0 else end + 3
        else:
            end = text.find('>', match.end())
            if end < 0:
                result.append(text[match.start():])
                return ''.join(result)
            pos = end + 1
            if text[end - 1] != '/':
                close = re.search(r'</' + match.group(1) + r'\s*>', text[pos:], re.I | re.ASCII)
                pos = len(text) if close is None else pos + close.end()
        result.append(' ')
    result.append(text[pos:])
    return ''.join(result)


def split_outer(text, separator, maxsplit=None):
    parts, start, i, braces, links = [], 0, 0, 0, 0
    while i < len(text):
        pair = text[i:i + 2]
        if pair == '{{':
            braces += 1
        elif pair == '}}' and braces:
            braces -= 1
        elif pair == '[[':
            links += 1
        elif pair == ']]' and links:
            links -= 1
        else:
            if text[i] == separator and not braces and not links and (maxsplit is None or len(parts) < maxsplit):
                parts.append(text[start:i])
                start = i + 1
            i += 1
            continue
        i += 2
    parts.append(text[start:])
    return parts


def templates(text):
    spans, stack, depth, i = [], [], 0, 0
    while i + 1 < len(text):
        pair = text[i:i + 2]
        if pair == '{{':
            depth += 1
            if depth <= 64:
                end = i + 2
                while end < len(text) and text[end] not in '|{}':
                    end += 1
                name = normalize(text[i + 2:end])
                opaque = bool(stack and stack[-1][1]) or name in ANNOTATIONS
                span = None
                if not opaque and name in TARGETS:
                    span = len(spans)
                    spans.append([i + 2, None])
                stack.append((span, opaque))
            i += 2
        elif pair == '}}' and depth:
            if depth <= 64:
                span, _ = stack.pop()
                if span is not None:
                    spans[span][1] = i
            depth -= 1
            i += 2
        else:
            i += 1
    for start, end in spans:
        if end is None:
            continue
        parts = split_outer(text[start:end], '|')
        params, positional = {}, []
        for part in parts[1:]:
            pair = split_outer(part, '=', 1)
            if len(pair) == 2:
                params[normalize(pair[0])] = pair[1]
            else:
                positional.append(part.strip())
        yield normalize(parts[0]), params, positional


def without_templates(value):
    output, depth, i = [], 0, 0
    while i < len(value):
        pair = value[i:i + 2]
        if pair == '{{':
            depth += 1
            i += 2
        elif pair == '}}' and depth:
            depth -= 1
            if not depth:
                output.append(' ')
            i += 2
        else:
            if not depth:
                output.append(value[i])
            i += 1
    return ''.join(output)


def parse_date(raw):
    value = without_templates(raw).strip()
    iso = re.fullmatch(r'([0-9]{4})-([0-9]{2})-([0-9]{2})', value)
    words = value.lower().replace(',', ' ').split()
    year, month, day = 0, 0, 0
    try:
        if iso:
            year, month, day = map(int, iso.groups())
            if not 1 <= month <= 12 or not 1 <= day <= 31:
                return None
        elif len(words) == 1 and len(words[0]) == 4:
            year = int(words[0])
        elif len(words) in (2, 3) and words[0] in MONTHS and len(words[-1]) == 4:
            year, month = int(words[-1]), MONTHS.index(words[0]) + 1
            if len(words) == 3:
                day = int(words[1])
                if not 1 <= day <= 31:
                    return None
        if year <= 0:
            return None
        if day:
            dt.date(year, month, day)
        return year, month, day
    except ValueError:
        return None


def parse_page(page_id, title, wikitext):
    ids, dates, majors = set(), [], set()
    vulkan = opengl = False
    for name, params, _ in templates(mask(wikitext)):
        if name == 'infobox game':
            for token in re.split(r'[\s,]+', without_templates(params.get('steam appid', ''))):
                if re.fullmatch(r'[1-9][0-9]*', token) and int(token) <= 0xffffffff:
                    ids.add(int(token))
            for row_name, _, positional in templates(params.get('release dates', '')):
                if row_name == 'infobox game/row/date' and len(positional) >= 2 and normalize(positional[0]) in ('windows', 'microsoft windows'):
                    date = parse_date(positional[1])
                    if date:
                        dates.append(date)
        elif name == 'api':
            majors.update(int(match.group(1)) for match in D3D.finditer(without_templates(params.get('direct3d versions', ''))))
            def present(key):
                value = normalize(without_templates(params.get(key, '')))
                return bool(value) and value not in ('false', 'no', 'none', 'unknown', 'n/a')
            vulkan |= present('vulkan versions')
            opengl |= present('opengl versions')
    backend = 'neptune' if majors & {11, 12} else 'mullion' if 10 in majors else 'venus'
    return {'pageID': page_id, 'title': title, 'appIDs': sorted(ids), 'windowsReleaseDate': min(dates) if dates else None, 'direct3DMajors': sorted(majors), 'hasVulkan': vulkan, 'hasOpenGL': opengl, 'backend': backend}


def candidate_key(candidate):
    date = candidate['windowsReleaseDate']
    return (date is not None, tuple(date or (0, 0, 0)), {'venus': 0, 'mullion': 1, 'neptune': 2}[candidate['backend']], -candidate['pageID'])


def make_output(candidates):
    result = {'schemaVersion': 1, 'generatedAt': dt.datetime.now(dt.timezone.utc).isoformat(), 'source': 'PCGamingWiki', 'neptune': [], 'mullion': [], 'venus': []}
    for app_id, candidate in candidates.items():
        result[candidate['backend']].append(int(app_id))
    for backend in ('neptune', 'mullion', 'venus'):
        result[backend].sort()
    return result


def atomic_json(path, data):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8') as stream:
        json.dump(data, stream, separators=(',', ':'), ensure_ascii=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


class Client:
    def __init__(self, proxy=None):
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({'http': proxy, 'https': proxy}) if proxy else urllib.request.ProxyHandler())
        self.last_request = 0

    def query(self, params):
        params = dict(params, action='query', format='json', formatversion='2')
        url = ENDPOINT + '?' + urllib.parse.urlencode(params)
        for attempt in range(8):
            time.sleep(max(0, 1.21 - (time.monotonic() - self.last_request)))
            self.last_request = time.monotonic()
            try:
                request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
                with self.opener.open(request, timeout=60) as response:
                    result = json.load(response)
                if 'error' in result:
                    raise RuntimeError('MediaWiki API error: ' + str(result['error']))
                return result
            except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as error:
                if isinstance(error, urllib.error.HTTPError) and error.code != 429 and not 500 <= error.code <= 599:
                    raise
                if attempt == 7:
                    raise
                delay = min(120, 2 ** attempt)
                if isinstance(error, urllib.error.HTTPError) and error.headers.get('Retry-After'):
                    retry = error.headers['Retry-After']
                    try:
                        delay = max(delay, float(retry))
                    except ValueError:
                        parsed = email.utils.parsedate_to_datetime(retry)
                        delay = max(delay, (parsed - dt.datetime.now(dt.timezone.utc)).total_seconds())
                print(f'Request failed ({error}); retrying in {delay:.1f}s', file=sys.stderr, flush=True)
                time.sleep(delay)


def run(args):
    checkpoint = Path(args.checkpoint)
    if checkpoint.exists():
        with checkpoint.open(encoding='utf-8') as stream:
            state = json.load(stream)
        if state.get('max_pages') != args.max_pages:
            raise ValueError('Checkpoint --max-pages differs; choose a different checkpoint')
        print('Resuming checkpoint', flush=True)
    else:
        state = {'max_pages': args.max_pages, 'page_ids': [], 'ei_continue': None, 'enumerated': False, 'batch': 0, 'candidates': {}, 'scanned': 0}
    client = Client(args.proxy)
    while not state['enumerated']:
        params = {'list': 'embeddedin', 'eititle': 'Template:API', 'einamespace': 0, 'eilimit': 500}
        if state['ei_continue']:
            params.update(state['ei_continue'])
        result = client.query(params)
        state['page_ids'].extend(page['pageid'] for page in result['query']['embeddedin'])
        if args.max_pages is not None and len(state['page_ids']) >= args.max_pages:
            state['page_ids'] = state['page_ids'][:args.max_pages]
            state['enumerated'] = True
        else:
            state['ei_continue'] = result.get('continue')
            state['enumerated'] = not bool(state['ei_continue'])
        atomic_json(checkpoint, state)
        print(f"Enumerated {len(state['page_ids'])} pages", flush=True)
    ids = state['page_ids']
    while state['batch'] * 50 < len(ids):
        batch_ids = ids[state['batch'] * 50:state['batch'] * 50 + 50]
        continuation = None
        seen = set()
        while True:
            params = {'pageids': '|'.join(map(str, batch_ids)), 'prop': 'revisions', 'rvprop': 'content', 'rvslots': 'main'}
            if continuation:
                params.update(continuation)
            result = client.query(params)
            for page in result['query']['pages']:
                revisions = page.get('revisions', [])
                if not revisions:
                    continue
                content = revisions[0].get('slots', {}).get('main', {}).get('content')
                if content is None:
                    raise RuntimeError(f"Missing revision content for page {page.get('pageid')}")
                candidate = parse_page(page['pageid'], page.get('title', ''), content)
                seen.add(page['pageid'])
                for app_id in candidate['appIDs']:
                    key = str(app_id)
                    previous = state['candidates'].get(key)
                    if previous is None or candidate_key(candidate) > candidate_key(previous):
                        state['candidates'][key] = candidate
            continuation = result.get('continue')
            if not continuation:
                break
        state['scanned'] += len(seen)
        state['batch'] += 1
        if state['batch'] % 5 == 0:
            atomic_json(checkpoint, state)
        print(f"Scanned {state['scanned']}/{len(ids)} pages; {len(state['candidates'])} AppIDs", flush=True)
    atomic_json(checkpoint, state)
    result = make_output(state['candidates'])
    encoded = json.dumps(result, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    if len(encoded) >= 20_000_000:
        raise ValueError('Index exceeds 20 MB')
    if not args.dry_run:
        atomic_json(args.output, result)
    print(f"Finished: {state['scanned']} pages, " + ', '.join(f"{key}={len(result[key])}" for key in ('neptune', 'mullion', 'venus')) + f', {len(encoded)} bytes', flush=True)
    # Keep checkpoint for dry runs; an interrupted or failed run must remain resumable.
    if not args.dry_run:
        checkpoint.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--output', default=str(Path(__file__).resolve().parents[2] / 'Resources/steam-graphics-index.json'))
    parser.add_argument('--checkpoint', default='/tmp/pcgw-index-checkpoint.json')
    parser.add_argument('--proxy')
    parser.add_argument('--max-pages', type=int)
    parser.add_argument('--dry-run', action='store_true', help='Do not write the output or delete the checkpoint')
    args = parser.parse_args()
    if args.max_pages is not None and args.max_pages <= 0:
        parser.error('--max-pages must be positive')
    run(args)


if __name__ == '__main__':
    main()
