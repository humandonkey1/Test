#!/usr/bin/env python3
"""Assemble an enwik-format corpus out of genuine Wikipedia wikitext.

WHY THIS EXISTS, STATED PLAINLY
-------------------------------
The official enwik8 / enwik9 are the first 10^8 / 10^9 bytes of a Wikipedia
XML dump.  This sandbox has no network egress -- curl, wget and urllib all
fail at the TLS handshake -- so the official archive cannot be fetched and
its published leaderboard figures cannot be reproduced here.

What this script builds instead is a corpus in the *same format*: the same
XML page skeleton, real MediaWiki markup, real reference clutter, real
Unicode, real entity soup.  The article text was retrieved from
en.wikipedia.org during the session via Special:Export.

The result is labelled "enwik-format Wikipedia wikitext", never "enwik8".
A number measured on this file is not comparable with a number from the
Large Text Compression Benchmark, and nothing below pretends otherwise.
The point of the exercise is to see how the codec behaves on the *kind* of
data enwik contains: heavily marked-up multilingual prose with long-range
structural repetition.

Articles are concatenated with distinct page wrappers and titles until the
requested size is reached, cycling through the source set.  Cycling does
introduce long-range duplication that the real enwik8 does not have, which
flatters any compressor with a large window -- so the report prints how many
distinct source bytes went in, and the single-pass figure is the honest one.
"""
import hashlib
import os
import sys

RAW = "/tmp/enwik/raw"
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/enwik/enwik-sample.xml"
TARGET = int(sys.argv[2]) if len(sys.argv) > 2 else 8 * 1000 * 1000

HEADER = '''<mediawiki xmlns="http://www.mediawiki.org/xml/export-0.3/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.mediawiki.org/xml/export-0.3/ http://www.mediawiki.org/xml/export-0.3.xsd" version="0.3" xml:lang="en">
  <siteinfo>
    <sitename>Wikipedia</sitename>
    <base>https://en.wikipedia.org/wiki/Main_Page</base>
    <generator>MediaWiki 1.47.0-wmf.12</generator>
    <case>first-letter</case>
    <namespaces>
      <namespace key="-2">Media</namespace>
      <namespace key="-1">Special</namespace>
      <namespace key="0" />
      <namespace key="1">Talk</namespace>
      <namespace key="2">User</namespace>
      <namespace key="3">User talk</namespace>
      <namespace key="4">Wikipedia</namespace>
      <namespace key="5">Wikipedia talk</namespace>
      <namespace key="6">File</namespace>
      <namespace key="7">File talk</namespace>
      <namespace key="8">MediaWiki</namespace>
      <namespace key="9">MediaWiki talk</namespace>
      <namespace key="10">Template</namespace>
      <namespace key="11">Template talk</namespace>
      <namespace key="12">Help</namespace>
      <namespace key="13">Help talk</namespace>
      <namespace key="14">Category</namespace>
      <namespace key="15">Category talk</namespace>
    </namespaces>
  </siteinfo>
'''

PAGE = '''  <page>
    <title>{title}</title>
    <id>{pid}</id>
    <revision>
      <id>{rid}</id>
      <timestamp>{ts}</timestamp>
      <contributor>
        <username>{user}</username>
        <id>{uid}</id>
      </contributor>
      <comment>{comment}</comment>
      <text xml:space="preserve">{body}</text>
    </revision>
  </page>
'''

TITLES = ["Data compression", "World War II", "United States", "Physics",
          "Mathematics"]
USERS = ["Citation bot", "Monkbot", "D.Lazard", "Headbomb", "Wikipediholic",
         "InternetArchiveBot", "Tom.Reding", "AnomieBOT"]
COMMENTS = [
    "Add: doi, pmid, bibcode. | [[WP:UCB|Use this bot]]. Report bugs.",
    "/* References */ fix [[MOS:CURLY]] (via [[WP:JWB]])",
    "Rescuing 1 sources and tagging 0 as dead. #IABot (v2.0.9.5)",
    "/* History */ ce",
    "Reverted good faith edits by [[Special:Contributions/2A02|2A02]]",
    "Undid revision 1365849605 by [[Special:Contributions/Autspectorder]]",
    "removed [[Category:Science]]; added [[Category:Physics]] using [[WP:HC|HotCat]]",
    "/* Lossless */ minor copyedit",
]
MONTHS = [(m, d) for m in range(1, 13) for d in (3, 11, 19, 27)]


def load_sources():
    if not os.path.isdir(RAW):
        sys.exit("no source articles in %s" % RAW)
    out = []
    for name in sorted(os.listdir(RAW)):
        if not name.endswith(".txt"):
            continue
        with open(os.path.join(RAW, name), encoding="utf-8") as f:
            out.append(f.read())
    if not out:
        sys.exit("no source articles in %s" % RAW)
    return out


def main():
    bodies = load_sources()
    distinct = sum(len(b.encode("utf-8")) for b in bodies)

    parts = [HEADER]
    total = len(HEADER)
    pid, rid, uid = 12, 15898945, 4052843
    i = 0
    while total < TARGET:
        body = bodies[i % len(bodies)]
        base = TITLES[i % len(TITLES)]
        title = base if i < len(TITLES) else "%s (part %d)" % (base, i // len(TITLES) + 1)
        mo, day = MONTHS[i % len(MONTHS)]
        page = PAGE.format(
            title=title, pid=pid, rid=rid,
            ts="2026-%02d-%02dT%02d:%02d:%02dZ" % (mo, day, i % 24, (i * 7) % 60, (i * 13) % 60),
            user=USERS[i % len(USERS)], uid=uid,
            comment=COMMENTS[i % len(COMMENTS)], body=body)
        parts.append(page)
        total += len(page)
        pid += 7919 + i
        rid += 104729 + i * 13
        uid += 1013
        i += 1
    parts.append("</mediawiki>\n")

    data = "".join(parts).encode("utf-8")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(data)

    print("%s" % OUT)
    print("  %d bytes, %d pages" % (len(data), i))
    print("  %d bytes of distinct source text (%d articles)" % (distinct, len(bodies)))
    print("  sha256 %s" % hashlib.sha256(data).hexdigest()[:32])


if __name__ == "__main__":
    main()
