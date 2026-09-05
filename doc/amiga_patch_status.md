# Patching du Rick Dangerous Amiga (`rickd.adf`) — statut

## Objectif

Pouvoir patcher, depuis l'éditeur, la « ROM » Amiga fournie dans
`Amiga Rick Dangerous 1/rickd.adf` (image disque ADF, 901120 octets =
880 Ko DD).

## Progrès émulation / RE (session FS-UAE 3.2.35)

### Configuration FS-UAE fonctionnelle (confirmée)
- **Passer la config en argument POSITIONNEL** : `fs-uae /chemin/fichier.fs-uae`
  (et PAS `--config=` : ce flag positionne un *chemin de recherche*, il ne
  charge pas la config à exécuter — c'était la cause du « floppy non montée »).
- Lancement headless sur `DISPLAY=:0` : binaire
  `/opt/FS-UAE/Linux/x86-64/fs-uae`, `LD_LIBRARY_PATH=/opt/FS-UAE/Linux/x86-64`.
- La disquette monte alors correctement :
  `gui_disk_image_change drive 0 name ...rickd.adf write protected 0`.
- Intégration UI : lancer le driver `tools/amiga_re/uae_run.py config fifo log pid`
  (pty = terminal de contrôle requis par le débugger console).

### Débugger UAE intégré (confirme l'entrée)
- `console_debugger = 1` dans la config **et** exécution depuis un terminal.
- **Entrée = Alt+D** (pas F12+D) : la doc officielle dit « Mod+D », le modificateur
  par défaut étant **Alt** sur Linux.
  (`tools/amiga_re/chord2.py <window-id>` l'envoie via XTEST/python-xlib.)
- Débugger pilotable par stdin/pty : `?` (aide), `r` (registres `D0..A7/USP/ISP`),
  `m addr [lignes]` (dump mémoire), `d addr` (désassemblage), `g` (reprend),
  `W addr <valeurs>` (écrire mémoire), `s`/`S` (recherche/sauvegarde mémoire),
  `w <n> <addr> <len> <R/W/I/F/C>` (watchpoint), `f` (breakpoints),
  `o`/`od`/`ot`/`ob` (Copper), `C`/`Cl`/`D` (trainers).
- Dump mémoire confirmé : vecteurs d'exception dans la RAM chip à `000000`
  pointent vers la KS ROM (`00FC....`), ~`00000060` : `00FC0C8E`, `00FC0CE2`…
  (tableau de sauts KS 1.3).
- L'écran headless est lisible via `tools/amiga_re/xshot.py` (capture ASCII
  bicolore `ascii` ou PNG/BMP) sur la fenêtre SDL (id `0xa00008`).

### Bloqueur principal (non résolu)
- **La disquette ne boote pas : aucune lecture disque.**
  - Bootblock `DOS\0` invalide (somme ≠ 0) : `44 4f 53 00 c0 20...`, somme sur
    512 mots = 0xB06C (≠ 0) → protection anti-copie du trainer pack.
  - Après correction de la somme de contrôle (somme=0, `rickd_fixed.adf`),
  **toujours aucune lecture `trackdisk`** dans les logs et écran figé au
  démarrage KS. → La K S 1.3 reste bloquée au boot (le reste du disque a aussi
  été corrompu/protégé : blocs FFS avec sommes invalides selon l'état initial).
- Conséquences : on ne peut pas laisser le jeu charger son moteur depuis le
  disque pour dumper la RAM (le loader n'aboutit pas).

### Mise à jour (session suivante) — nouveaux faits vérifiés
- **La disquette peut booter en headless : le souci est propre à chaque ADF.**
  Un pack cracké de référence (`Rick Dangerous & Rick Dangerous 2 (2006)
  (Flashtro)[cr Flashtro][t Flashtro]`, téléchargé d'archive.org) **démarre et
  affiche son écran d'intro** sous ce FS-UAE headless (capture ASCII visible :
  logo « RICK DANGEROUS », fond décoratif). Donc le moteur DF0 se met bien en
  marche et le code tourne quand le disque est sain.
- Les **deux** ADF (`rickd.adf` et `flashtro.adf`, tous deux 901120 o) utilisent
  un **layout DOS non standard / protégé** : `xdftool` échoue (« Invalid Root
  Block »), l'extracteur manuel ne trouve rien (0 octet). L'écran d'intro
  s'affiche mais **reste figé** (aucune avancée à Espace/Entrée) : chargement
  bloqué à un étage de lecture disque du loader crack.
- `flashtro.adf` ≠ `rickd.adf` (déjà à l'octet 5) : ce dernier a un bootblock
  obfusqué, l'autre un loader 68k propre mais non-DOS.
- **Débugger UAE pleinement fonctionnel** : Alt+D, `W addr ...` écrit la RAM
  chip, `r PC <addr>` positionne le compteur programme, `g`/`r`/`m`/`c` OK.
- **Stub d'affichage injecté** (`tools/amiga_re/inject_neo.py`) : un petit 68k
  écrit en RAM chip (code `0x70000`, image `twi2.neo` en `0x74000`) qui configure
  l'écran 320×200, 4 bitplanes pour afficher la bitmap. La RAM écrit bien ;
  reste à fiabiliser la prise de contrôle de l'affichage par le stub.

### Prochaines pistes (si on reprend)
1. **Contourner le boot DOS** : injecter directement en RAM émulée via le
   débugger (`W addr ...`) les fichiers déjà extraits (`twi2.neo`, `panel.dlt`,
   binaires `blk917`/`blk890`, table d'offsets), puis `g` et set des watchpoints
   pour capturer le code qui lit `panel.dlt` et fait la collision.
2. **Finir de valider le rendu** via le stub injecté (`inject_neo.py`) : confirmer
   que `twi2.neo` s'affiche (4 planes 320×200 + palette 16), puis étendre à
   `panel.dlt` pour vérifier visuellement le format de deltas renseigné dans la doc.
3. Désassemblage statique hors émulateur (Ghidra/capstone m68k) des binaires
   extraits avec un loader hunk, en se concentrant sur le code `Open`/`Read`
   (`dos.library`) qui charge `panel.dlt`, et la table d'offsets finale du
   binaire (`00 00 03 EC, 00 00 05 4F, ...`) qui indexe les sous-blocs.

*Le verdict structurel du document reste valide : le monde Amiga est du
graphisme delta-compressé (`twi2.neo` bitmap + `panel.dlt` deltas), pas une
grille de tuiles éditable équivalente à `map_bnums`.*

## Conclusion en bref

**Pas possible tel quel.** L'éditeur et son patcher (`src/xrick_patch.h`)
travaillent sur le format de **xrick, le port DOS**, alors que l'ADF
contient le **Rick Dangerous 1 Amiga original** (port 68k de Twilight /
Enigmasoft, copyright « T.BARKER 8/9/88 »). Deux verrous distincts :

1. **Format de fichier** : le patcher ne sait lire que des exécutables
   `ELF32/ELF64` (Linux) et `PE32` (Windows), en localisant les tables
   par symboles/signatures. Une `.adf` est une image de disque avec un
   filesystem AmigaDOS, pas un exécutable. Il n'y a aucun parseur ADF.

2. **Format de données** : les tables éditées (`map_bnums`, tuiles 4bpp,
   sprites, `map_connect`, `map_submaps`, `map_marks`) correspondent au
   layout précis du `dat_maps.c` / `dat_ents.c` du port DOS. L'Amiga
   stocke ses graphismes en **planaires** (Neochrome `.neo`) et ses
   niveaux dans un format **packé maison** (`.dlt`), sans aucun rapport.
   Les signatures du xrick DOS (`map_bnums`, écran `@SOUTH@AMERI…`) sont
   **absentes** de l'ADF.

## Ce qui a été déterminé sur le disque

- `rickd.adf` : ADF 880 Ko standard (80 cylindres × 2 faces × 11 secteurs
  × 512 o). Bootblock `DOS` présent (magic `44 4f 53 00`).
- Le bootblock déclare `DOS0` (**OFS**), mais les blocs ont des sommes de
  contrôle de **FFS** (bloc entier) : cette incohérence = protection
  anti-copie / disque modifié. Le montage par `amitools.ADFSVolume` échoue
  donc à l'ouverture (racine invalide).
- Racine à **bloc 880** (position DD 88Ko standard), mais entièrement
  réécrite en blocs de données. **Hiérarchie aplatie** : les blocs de
  répertoire (type 2) possèdent des blocs de données (type 8) accrochés
  directement (champ parent `L1` = n° de répertoire, `L2` = séquence,
  `L4` = suivant), **sans blocs d'en-tête de fichier séparés** (`type -3`
  absent à part les marqueurs de fin `fffffffd`). Ex. : `twi2.neo`
  = répertoire 913 → blocs de données ~407-472. C'est une réorganisation
  volontaire anti-copie du filesystem, pas un FFS standard.
- Les blocs de répertoire (type 2) et de données (type 8) restent
  lisibles. L'inventaire complet des fichiers a été extrait (cf. `extract_adf.py`).

### Inventaire des fichiers (racine du disque) — disque crack/trainer pack

Analyse par extraction de chaînes ASCII : ce disque est un **pack crack /
trainer de « TOP SWAP »/« Sensei » (1989)** (« Smash F1 pour Rick
Dangerous, F2 Twylyte, F3 Coder board », textes retrouvés dans `allsounds`
et `2`). Il boote sur AmigaOS et embarque des utilitaires système + les
données du jeu.

- `Startup-sequence`, `SENSEI`, `blk890/930/917/976` — code partagé
  (binaire du menu/trainer + chargeur), signature commune `a4ejr a(e<r BTCa`.
- `blk885`/`blk972` — **programme Keymap** (`keymap.resource`,
  `console.device`, `DEVS:keymaps/`, `icon.library`).
- `blk888` — **Copy** (`#FROM/A,TO/S,OPT/K,HEX/S…`, « Can't open %S »).
- `blk974` — **Disk-Validator** (« bitmap checksum error », « bad block
  type », « second root block », « bad directory/header »).
- `blk978` — **Ram-Handler** (« RAM: »). `blk1001` — référence `DiskCopy`.
- `blk969` — trainer/ShowBoot (clés + « Serial No »).
- `blk957` — note : « TheBand ; Trainermenu / Twylyte ; mainprogram /
  Capone + Ice Brigade (Roxy) = GAYS! »
- `allsounds`, `2` — textes (instructions, « Sensei 1989 »).
- `twi2.neo`, `panel.dlt`, `pre.neo`, `title.dlt` — graphismes/écrans du jeu.
- `music.spl`, `combined.spl`, `allsounds`, `theband` — audio (`.spl`).

Programme de chargement : `intro` (chaîne `VERSION 1.0COPYRIGHT
T.BARKER 8/9/88 ENIGMASOFT`), charge `dos.library`, `exec.library`,
`graphics.library`, `intuition.library`, `input.device` puis lit les
données directement.

## Écart de format — pourquoi pas de correspondance directe

| Élément | xrick (édition actuelle) | Amiga (disque) |
|---|---|---|
| Code | C (port PC) | 68k assembly original (Twilight) |
| Graphismes | tuiles 4 bpp/palette (`tile_t = U32[8]`) | planaires bitplanes, images `.neo` |
| Niveaux | `map_bnums` (U8), `map_connect`, `map_submaps`, `map_marks`, `dat_ents.c` | `.dlt` packé maison |
| Cible de patch | exécutable ELF/PE | image disque ADF (filesystem) |
| Symboles | tables ELF/COFF ou signatures | aucun (hunks 68k) |

Même une fois les fichiers extraits (fait, cf. ci-dessous), il faudrait
**réimplémenter/désassembler entièrement le moteur 68k** pour savoir
comment `panel.dlt` / `twi2.neo` encodent la carte, les objets et le décor,
et écrire un convertisseur bidirectionnel. Impossible de fournir un patch
fiable sans désassemblage + vérification sous émulateur (WinUAE/FS-UAE).

## Outil fourni

`tools/extract_adf.py` — extrait les fichiers de l'ADF :

- monte le volume via `amitools` quand c'est possible (disques standards) ;
- sinon (FFS aplati anti-copie, cas `rickd.adf`), reconstitue le contenu
  en triant les blocs de données (type 8) par leur **n° de séquence** L2
  (le champ parent L1 = bloc du fichier, les données = octets 24..).

Usage :

```
python3 tools/extract_adf.py <chemin.adf> [dossier_sortie]
pip install amitools   # dépendance
```

Résultat sur `rickd.adf` : **620 Ko de données extraits**, dont les fichiers
du jeu : `twi2.neo`, `panel.dlt`, `pre.neo`, `title.dlt`, `music.spl`,
`combined.spl`, `allsounds`, `theband`, `SENSEI`, et les utilitaires /
exécutables 68k (`blk885/888/890/917/937/976/978…` = `loadseg()` AmigaOS).

## Format des données du jeu (identifié, mais non éditable directement)

Après extraction, les formats réels des données Amiga ont été identifiés —
ils sont **différents de ceux du port DOS** :

| Fichier | Taille | Nature |
|---|---|---|
| `twi2.neo` | 32 Ko | en-tête 128 o + **bitmap planaire 320×200 4 bitplanes (32000 o)** + palette 16 couleurs. Image plein écran (titre/niveau), pas les données de carte. |
| `panel.dlt` | 77 Ko | données « delta » (`.dlt`) : **flux d'enregistrements `{addresse 0x3E8+k, mots de données…}` qui patchent des bitplanes** (`03ec 01fe 01ff / 03f0 80c4 80c4 / …`). C'est un format d'écran/façade delta, **pas une carte de tuiles**. |
| `music.spl` / `combined.spl` / `allsounds` | 33/27/0,6 Ko | modules sons (`.spl` = sample). `combined.spl` est suivi dans le binaire du jeu par une table d'offsets (0x08,0x14,0x20,0x2C…). |
| binaires `loadseg()` | 68k | essentiellement des **utilitaires AmigaOS** (Keymap, Copy, DiskValidator, RamHandler, DiskCopy) + le menu/trainer commun. Les exécutables 68k sont de vrais fichiers hunk (`0x3F3`), vérifiables avec capstone (désassemblage m68k). |

Les données de **carte/objets** (`map_bnums` équivalent) : le binaire du
jeu (`blk917`) référence `TWYLYTE:panel.dlt`, `TWYLYTE:twi2.neo`,
`TWYLYTE:allsounds`, … via une table. `twi2.neo` = bitmap plein écran,
`panel.dlt` = **delta-patch de bitplanes**. Le binaire lui-même se termine
par une **immense table d'offsets croissants** (`00 00 03 ec, 00 00 05 4f,
…`) indexant des sous-blocs compressés, puis `0x3F2` (HUNK_END) / `0x3EB`
(HUNK_BSS).

**Conclusion structurelle** : le « monde » Amiga est encodé en **graphismes
delta-compressés** (`twi2.neo` bitmap + `panel.dlt` deltas), rendus en
bitplanes. Il n'existe **pas de grille de tuiles éditable**
(équivalent `map_bnums`).

*Nuance RE (désassemblage capstone de `blk917`, le shell menu/titre) :*
le moteur manipule ses objets/entités via **structures 16/32 bits**
(`move.w`/`move.l` indirects), **sans lecture de pixel individuel**
(`move.b (aN)` quasi absent, pas de test de couleur). La collision semble
donc **guidée par des données** (structures/tableaux), pas par lecture de
couleurs dans la bitmap — ce serait plus éditable qu'un modèle pixel pur.
MAIS : l'engagement n'est pas confirmé — le code de chargement/lecture
des fichiers (`Open`/`Read` dos.library) et le code de collision complet
**n'ont pas été localisés** dans les binaires disponibles (`blk917` n'a
qu'un `Close` ; `blk890`, 190 Ko, n'a ni chaînes ni appels dos). Le vrai
moteur qui lit `panel.dlt` et teste la collision n'est pas identifié
proprement (le disque est un pack crack/trainer ; le moteur est
probablement fortement entrelacé données/code, ce qui casse le
désassemblage linéaire).

## Feuille de route réaliste pour un vrai « patch Amiga » depuis l'éditeur

1. ~~Extraction fiable~~ **DONE** : l'outil reconstitue déjà les chaînes de
   blocs du FFS aplati et sort tous les fichiers (620 Ko).
2. **Reverse du moteur** (Ghidra + loader 68k, ou émulateur + dump mémoire)
   pour décoder **la compression** (`panel.dlt` deltas, table d'offsets) et
   **les règles pixel→collision**, puis produire le mapping vers le modèle
   de l'éditeur. C'est LE gros morceau — le monde est du graphisme,
   pas une grille.
3. **Convertisseur bidirectionnel** DOS-xrick ↔ Amiga.
4. **Éditeur ADF** : réécrire les blocs du filesystem + recalculer les
   sommes de contrôle FFS.
5. **Intégration UI** : nouveau menu « Patch Amiga » réutilisant le pipeline
   `PatchResult` existant.
6. **Vérification** : round-trip + test sous émulateur Amiga.

Les étapes 2-3 sont le gros du travail et dépassent le cadre d'une session
d'édition (elles nécessitent désassemblage et exécution sous émulateur).
**Recommandation** : pour un éditeur de niveaux Amiga, mieux vaut re-partir
d'un modèle pixel/tuiles propre (au-dessus de `twi2.neo`+`panel.dlt`), dans un
outil séparé, plutôt que de router l'éditeur xrick existant.
