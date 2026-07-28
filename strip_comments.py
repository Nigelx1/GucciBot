#!/usr/bin/env python3
"""Strip ALL C/C++ comments from every source file under a directory.

String-aware: '//...' and the URL '//' inside string literals are preserved,
as are comments inside raw string literals R"(...)" (e.g. GLSL shader source).
C++14 digit separators (1'000'000) and escape sequences round-trip verbatim.
Raw-string contents are NEVER modified -- not even trailing whitespace.

No git in this project -> caller is expected to have backed up src/ already.
"""
import os
import re
import sys

EXTS = ('.cpp', '.hpp', '.h', '.c', '.cc', '.cxx', '.hh', '.inl', '.ipp')
RAW_RE = re.compile(r'(?:^|[^A-Za-z0-9_])((?:u8|u|U|L)?R)$')


def strip_comments(src: str) -> str:
    crlf = '\r\n' in src
    s = src.replace('\r\n', '\n')
    out = []
    i, n = 0, len(s)
    st = 'N'  # N normal, L line comment, B block comment, S string, C char, R raw
    raw_close = ''
    raw_spans = []      # (start, end) byte ranges in `out` that are raw-string content
    rstart = 0
    emitted_on_line = False

    while i < n:
        c = s[i]
        nxt = s[i + 1] if i + 1 < n else ''

        if st == 'N':
            if c == '/' and nxt == '/':
                st = 'L'; i += 2; continue
            if c == '/' and nxt == '*':
                st = 'B'; i += 2; continue
            if c == '"':
                tail = ''.join(out[-4:]) if out else ''
                if RAW_RE.search(tail):
                    rstart = len(out)
                    out.append('"'); i += 1
                    d = ''
                    while i < n and s[i] not in '(\n' and len(d) < 20:
                        d += s[i]; out.append(s[i]); i += 1
                    if i < n and s[i] == '(':
                        out.append('('); i += 1
                    raw_close = ')' + d + '"'
                    st = 'R'
                else:
                    out.append('"'); st = 'S'; emitted_on_line = True; i += 1
                continue
            if c == "'":
                out.append("'"); st = 'C'; emitted_on_line = True; i += 1; continue
            out.append(c)
            if c == '\n':
                emitted_on_line = False
            elif not c.isspace():
                emitted_on_line = True
            i += 1; continue

        if st == 'L':
            if c == '\\' and nxt == '\n':
                i += 2; continue  # line continuation extends the comment
            if c == '\n':
                if emitted_on_line:
                    out.append('\n')
                emitted_on_line = False
                st = 'N'
            i += 1; continue

        if st == 'B':
            if c == '*' and nxt == '/':
                st = 'N'; i += 2; continue
            i += 1; continue

        if st == 'S':
            if c == '\\' and nxt:
                out.append(c); out.append(nxt); i += 2; continue
            out.append(c)
            if c == '"':
                st = 'N'
            elif c == '\n':
                emitted_on_line = False
            i += 1; continue

        if st == 'C':
            if c == '\\' and nxt:
                out.append(c); out.append(nxt); i += 2; continue
            out.append(c)
            if c == "'":
                st = 'N'
            elif c == '\n':
                emitted_on_line = False
            i += 1; continue

        if st == 'R':
            if raw_close and s.startswith(raw_close, i):
                out.append(raw_close); i += len(raw_close)
                raw_spans.append((rstart, len(out)))
                st = 'N'; continue
            out.append(c)
            if c == '\n':
                emitted_on_line = False
            i += 1; continue

    text = ''.join(out)

    # mark every byte that belongs to a raw-string literal
    raw_mask = bytearray(len(text))
    for rs, re_ in raw_spans:
        for k in range(rs, min(re_, len(raw_mask))):
            raw_mask[k] = 1

    # raw-aware cleanup: rstrip + blank-collapse ONLY on lines that contain
    # no raw-string bytes (including their terminating newline).
    parts = text.split('\n')
    protected = []
    offset = 0
    for p in parts:
        seg = raw_mask[offset:offset + len(p) + 1]  # +1 catches the trailing newline
        protected.append(any(seg))
        offset += len(p) + 1

    result_lines = []
    prev_blank = False
    for p, prot in zip(parts, protected):
        if prot:
            result_lines.append(p)        # verbatim, never rstrip, never collapse
            prev_blank = False
        else:
            ps = p.rstrip()
            if ps == '' and prev_blank:
                continue                  # collapse consecutive blanks
            result_lines.append(ps)
            prev_blank = (ps == '')

    while result_lines and result_lines[0] == '':
        result_lines.pop(0)
    while result_lines and result_lines[-1] == '':
        result_lines.pop()

    result = '\n'.join(result_lines) + '\n'
    if crlf:
        result = result.replace('\n', '\r\n')
    return result


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'src'
    rows = []
    t_old = t_new = 0
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith(EXTS):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, 'r', encoding='utf-8') as f:
                orig = f.read()
            stripped = strip_comments(orig)
            ol, nl = orig.count('\n'), stripped.count('\n')
            ob, nb = len(orig), len(stripped)
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(stripped)
            rel = os.path.relpath(path, root)
            rows.append((rel, ol, nl, ob, nb))
            t_old += ob; t_new += nb

    rows.sort(key=lambda r: -r[3])
    print(f'{"file":40s} {"old-L":>8s} {"new-L":>8s} {"old-B":>9s} {"new-B":>9s}')
    print('-' * 80)
    for rel, ol, nl, ob, nb in rows:
        print(f'{rel:40s} {ol:8d} {nl:8d} {ob:9d} {nb:9d}')
    print('-' * 80)
    print(f'TOTALS  files={len(rows)}  old={t_old}B  new={t_new}B  '
          f'removed={t_old - t_new}B ({100 * (t_old - t_new) / max(1, t_old):.1f}%)')


if __name__ == '__main__':
    main()
