"""Builds the dev game folders from the extracted discs (hardlinks, no copies):
  game/hd = what the game's installer (install.exe, eng.lis) puts on the hard disk
  game/cd = all four CDs merged (disc 1's data/cd.ini; the disc check: src/rt/ring_game.c)
usage: python tools/mkgame.py <original dir with disc1..disc4> [lang=eng]"""
import os, sys, fnmatch, shutil

BS = chr(92)


def ci_find(root, rel):
    """case-insensitive path lookup -> real path or None"""
    cur = root
    for part in rel.replace(BS, '/').split('/'):
        if not part:
            continue
        hit = None
        try:
            for e in os.listdir(cur):
                if e.lower() == part.lower():
                    hit = e
                    break
        except OSError:
            return None
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        return
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def main():
    orig = sys.argv[1]
    lang = sys.argv[2] if len(sys.argv) > 2 else 'eng'
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hd = os.path.join(root, 'game', 'hd')
    cd = os.path.join(root, 'game', 'cd')
    d1 = os.path.join(orig, 'disc1')
    lines = open(ci_find(d1, 'install/%s.lis' % lang), encoding='latin-1').read().split('\n')[1:]
    n = 0
    for ln in lines:
        p = ln.strip().rsplit(None, 1)
        if len(p) != 2:
            continue
        pat, code = p[0].replace(BS + BS, BS), p[1]
        if code not in ('1', '17'):          # 18 = install\ helpers, 19 = uninstall dirs
            continue
        d, f = pat.rsplit(BS, 1) if BS in pat else ('', pat)
        sd = ci_find(d1, d) if d else d1
        if not sd:
            print('missing dir', d)
            continue
        for e in os.listdir(sd):
            full = os.path.join(sd, e)
            if os.path.isfile(full) and fnmatch.fnmatch(e.lower(), f.lower()):
                if code == '17':             # language file -> data\<name>
                    dst = os.path.join(hd, 'data', e.lower())
                else:
                    dst = os.path.join(hd, d.lower().replace(BS, os.sep), e.lower())
                link(full, dst)
                n += 1
    print('hd: %d files' % n)
    m = 0
    for i in (1, 2, 3, 4):
        dd = os.path.join(orig, 'disc%d' % i)
        for base, dirs, files in os.walk(dd):
            for f in files:
                rel = os.path.relpath(os.path.join(base, f), dd).lower()
                if rel == os.path.join('data', 'cd.ini') and i != 1:
                    continue                 # disc 1's "1" answers for all (src/rt/ring_game.c)
                link(os.path.join(base, f), os.path.join(cd, rel))
                m += 1
    print('cd: %d files' % m)


main()
