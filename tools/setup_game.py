"""Build the game folder from the player's two Robot City CDs.

usage: python tools/setup_game.py <disc 1 folder or drive> <disc 2 folder or drive>

  game\\cd       = both discs merged (D:\\ for the game; disc 1 wins where both
                  have a file)
  game\\hd\\RC    = what the original setup put on the hard disk: ROBOTCTY.EXE,
                  SCIFI.EXE and RCP.DAT (the CD drive letter, D:\\)

Files are hard-linked when the discs are folders on the same drive, copied
otherwise.  The CD folders are only read.
"""
import os, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def link_or_copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def merge(disc, out):
    n = 0
    for base, dirs, files in os.walk(disc):
        for f in files:
            s = os.path.join(base, f)
            link_or_copy(s, os.path.join(out, os.path.relpath(s, disc)))
            n += 1
    return n


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d1, d2 = sys.argv[1], sys.argv[2]
    for d, tag in ((d1, 'RCID.000'), (d2, 'RCID.001')):
        if not os.path.exists(os.path.join(d, tag)):
            sys.exit('%s does not look like Robot City disc %s (no %s)' % (d, tag[-1], tag))
    cd = os.path.join(ROOT, 'game', 'cd')
    hd = os.path.join(ROOT, 'game', 'hd', 'RC')
    n = merge(d2, cd) + merge(d1, cd)          # disc 1 last: it wins
    for f in ('ROBOTCTY.EXE', 'SCIFI.EXE'):
        link_or_copy(os.path.join(cd, 'RC', f), os.path.join(hd, f))
    with open(os.path.join(hd, 'RCP.DAT'), 'wb') as fo:
        fo.write(b'D:' + bytes([92]) + b'\r\n\r\n')
    print('game folder ready: %d files from the discs' % n)


if __name__ == '__main__':
    main()
