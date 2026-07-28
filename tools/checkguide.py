#!/usr/bin/env python3
"""
Check an AmigaGuide file before it goes out.

    tools/checkguide.py NextSync.guide

AmigaGuide has no validator to hand and a broken one fails quietly -- a
link to a node that is not there gives the reader a dead end rather than an
error, and a missing @endnode swallows everything after it. This looks for
the mistakes that actually happen:

  - @database on the first line
  - every @node matched by an @endnode, none nested
  - node names unique
  - every link pointing at a node that exists
  - every node reachable from somewhere
  - no tabs (they render at whatever width the reader feels like)

Exits non-zero and lists what is wrong.
"""

import re
import sys

LINK = re.compile(r'@\{"[^"]*"\s+link\s+([A-Za-z0-9_.-]+)\s*\}')
NODE = re.compile(r'^@node\s+([A-Za-z0-9_.-]+)')


def check(path):
    with open(path, encoding='latin-1') as f:
        lines = f.read().split('\n')

    problems = []
    nodes, links, open_node, first = {}, [], None, None

    if not lines or not lines[0].startswith('@database'):
        problems.append("line 1 is not @database")

    for n, line in enumerate(lines, 1):
        if '\t' in line:
            problems.append("line %d has a tab" % n)

        m = NODE.match(line)
        if m:
            if open_node:
                problems.append("line %d: @node %s inside @node %s"
                                % (n, m.group(1), open_node))
            if m.group(1) in nodes:
                problems.append("line %d: @node %s is defined twice"
                                % (n, m.group(1)))
            nodes[m.group(1)] = n
            open_node = m.group(1)
            first = first or m.group(1)

        elif line.startswith('@endnode'):
            if not open_node:
                problems.append("line %d: @endnode with no @node" % n)
            open_node = None

        for target in LINK.findall(line):
            links.append((target, n, open_node))

    if open_node:
        problems.append("@node %s is never closed with @endnode" % open_node)

    for target, n, _ in links:
        if target not in nodes:
            problems.append("line %d: link to %s, which is not a node"
                            % (n, target))

    linked = {t for t, _, _ in links}
    for name in nodes:
        if name != first and name not in linked:
            problems.append("@node %s cannot be reached from any link" % name)

    return problems, nodes


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: checkguide.py <file.guide>")

    problems, nodes = check(sys.argv[1])
    for p in problems:
        print("  %s" % p)
    if problems:
        sys.exit(1)
    print("  guide: %d nodes, links all resolve" % len(nodes))


if __name__ == '__main__':
    main()
