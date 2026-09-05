#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# extract_adf.py -- extrait les fichiers d'une image ADF (AmigaDOS FF/OFS).
#
# 1) Essaie de monter le volume via amitools (disques standards).
# 2) Si le montage échoue (disque protégé / FFS modifié, cf. rickd.adf),
#    bascule en extraction manuelle des fichiers : sur un FFS « aplati »
#    anti-copie, chaque fichier = un bloc de répertoire/en-tête possédant
#    des blocs de données (type 8) dont le champ parent = n° du bloc chef,
#    et un n° de séquence L2 (1..n) qui donne l'ordre. On reconstitue donc
#    le contenu en triant les blocs par séquence (données = octets 24..).
#
# Dépendance :  pip install amitools
#
# Usage :
#   python3 extract_adf.py <image.adf> [dossier_sortie]
import sys, os, struct, shutil

def usage():
    print(__doc__)
    sys.exit(1)

def main():
    if len(sys.argv) < 2:
        usage()
    adf = os.path.abspath(sys.argv[1])
    outdir = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else "extracted_adf"
    if not os.path.isfile(adf) or os.path.getsize(adf) < 1024:
        print("Fichier ADF invalide :", adf); sys.exit(1)

    ok = try_amitools(adf, outdir)
    if not ok:
        print("Montage amitools impossible -> extraction manuelle (FFS aplati).")
        extract_flattened(adf, outdir)

def try_amitools(adf, outdir):
    try:
        from amitools.fs.blkdev.BlkDevFactory import BlkDevFactory
        from amitools.fs.ADFSVolume import ADFSVolume
    except ImportError:
        print("amitools non installé (pip install amitools).")
        return False

    work = adf + ".work%d.adf" % os.getpid()  # .adf exigé par amitools (détection par extension)
    shutil.copy(adf, work)  # ne jamais modifier l'original
    try:
        factory = BlkDevFactory()
        blkdev = factory.open(work)
        vol = ADFSVolume(blkdev)
        if not vol.open():
            return False
        os.makedirs(outdir, exist_ok=True)

        def walk(d, prefix=""):
            for e in d.get_entries():
                name = e.name.get_unicode()
                p = os.path.join(outdir, prefix, name)
                if e.is_dir():
                    os.makedirs(p, exist_ok=True)
                    walk(e, os.path.join(prefix, name))
                elif e.is_file():
                    data = e.get_file_data()
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    with open(p, "wb") as f: f.write(data)
                    print("extrait :", prefix + "/" + name, "(%d o)" % len(data))
                e.flush()
        walk(vol.get_root_dir())
        return True
    finally:
        try: blkdev.close()
        except Exception: pass
        os.remove(work)

def extract_flattened(adf, outdir):
    """Extraction manuelle sur un FFS aplati (protection anti-copie)."""
    data = open(adf, "rb").read()
    FS = 5632  # track 0 = bootblock (AmigaDOS)
    n = (len(data) - FS) // 512
    def L(i, o): return struct.unpack(">i", data[FS + i*512 + o*4 : FS + i*512 + o*4 + 4])[0]
    def name_of(blkno):
        b = data[FS + blkno*512 : FS + blkno*512 + 512]
        off = 0x1b0
        ln = b[off]
        if off + 1 + ln <= 512 and 1 <= ln < 26 and all(32 <= b[off+1+k] < 127 for k in range(ln)):
            return "".join(chr(b[off+1+k]) for k in range(ln))
        return None

    from collections import defaultdict
    par = defaultdict(list)
    for i in range(n):
        if L(i, 0) == 8:
            h = L(i, 1); s = L(i, 2); z = L(i, 3); nx = L(i, 4)
            if 0 <= h < 2000 and 0 <= nx < 2000 and 0 <= s < 2000 and 0 <= z <= 500:
                par[h].append((s, z, i))

    os.makedirs(outdir, exist_ok=True)
    total = 0
    for h in sorted(par):
        byseq = {x[0]: x for x in par[h]}
        seqs = sorted(byseq)
        cont = (seqs == list(range(1, max(seqs) + 1)))
        blobs = []
        for s in range(1, max(seqs) + 1):
            if s not in byseq: continue
            z, blkno = byseq[s][1], byseq[s][2]
            b = data[FS + blkno*512 : FS + blkno*512 + 512]
            blobs.append(b[24 : 24 + z])
        content = b"".join(blobs)
        nm = name_of(h) or ("blk%d" % h)
        nm = nm.replace("/", "_")
        with open(os.path.join(outdir, nm), "wb") as f:
            f.write(content)
        total += len(content)
        print("  %-18s parent %4d  blocs=%3d  seq_continu=%s  taille=%6d o"
              % (nm, h, len(par[h]), cont, len(content)))
    print("Total extrait : %d octets." % total)
    print("Voir doc/amiga_patch_status.md pour le format des données.")

if __name__ == "__main__":
    main()
