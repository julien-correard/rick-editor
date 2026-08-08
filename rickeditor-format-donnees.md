# RickEditor — analyse du format de données et du système de coordonnées

Document de référence avant portage. Objectif : que le comportement du binaire
porté soit strictement identique à celui du binaire Windows/MinGW d'origine
(cbp `tfg`), y compris ses bizarreries.

Base technique : moteur dérivé de **xrick** (BigOrno, 1998-2002), variante
Atari ST (`GFXST`), avec un éditeur de niveau maison (`main.cpp` / `main.h`).

---

## 1. Constat important : deux copies des données de tuiles

`dat_tilesST.c` (110 Ko, licence xrick BigOrno) et le tableau `tiles_data`
intégré dans `main.h` (lignes ~1061-8752) contiennent **exactement les mêmes
octets de données de tuiles** (diff strict après normalisation des fins de
ligne = seules la ligne de déclaration et le footer `#endif` diffèrent).

Cependant :

- **`dat_tilesST.c` n'est actuellement PAS compilé.** Le projet Code::Blocks
  `tfg.cbp` ne référence que `main.cpp`, `main.h` et `cb.bmp` comme unités.
  `main.cpp` n'inclut que `<SDL/SDL.h>` et `main.h` — jamais `tiles.h`,
  `system.h` ni `dat_tilesST.c`. `system.h`, `img.h`, `rects.h`, `tiles.h`,
  `config.h` ne sont utilisés par aucun symbole de `main.cpp`/`main.h`
  (`rect_t`, `img_t`, `img_color_t`, `IMG_SPLASH`, `tile_t` : zéro occurrence
  en dehors de ces headers eux-mêmes et de `dat_tilesST.c`).
- La donnée **réellement utilisée à l'exécution** est la copie locale de
  `main.h` : `unsigned long int tiles_data[4][0x100][8]`.
- `dat_tilesST.c`, lui, déclare `tile_t tiles_data[TILES_NBR_BANKS][0x100]`
  avec `TILES_NBR_BANKS = 3` (défini dans `tiles.h` pour `GFXST`).

`dat_tilesST.c` est donc la copie de référence « format d'origine xrick »,
orpheline du point de vue du *build* ; `main.h` est la copie « vivante »,
avec une différence structurelle :

- Le tableau de `main.h` est déclaré avec **4** banques, mais seuls 3 blocs
  de données sont fournis dans l'initialiseur (`BANK 0`, puis `BANK 2` — le
  commentaire précise que l'ancienne `BANK 1` a été supprimée car copie
  exacte de `BANK 0` —, puis `BANK 3`). La 4ᵉ banque du tableau (indice 3)
  n'est initialisée par aucun bloc : le compilateur C la remplit donc de
  zéros. C'est une banque « fantôme », inerte, qui n'existe pas dans
  `dat_tilesST.c` (dimensionné correctement à 3).

**Conséquence sur l'indexation** : les libellés `/* BANK 0/2/3 */` dans le
code sont des reliquats de la numérotation *avant* suppression de l'ancienne
banque 1. Une fois la banque 1 retirée, les données se retrouvent tassées
aux index physiques 0, 1, 2 :

| Indice réel `tiles_data[i]` | Contenu (libellé d'origine) |
|---|---|
| 0 | ancienne « BANK 0 » |
| 1 | ancienne « BANK 2 » |
| 2 | ancienne « BANK 3 » |
| 3 (main.h seulement) | vide (zéros), jamais généré par aucun bloc de données |

---

## 2. Format d'une tuile (`tile_t` / `tiles_data[bank][tile_num]`)

Une tuile = 8 valeurs `unsigned long` (32 bits), une par ligne de pixels
(tuile 8×8). Chaque valeur, **lue comme un littéral hexadécimal à 8 chiffres
avec zéros de tête explicites** (ex. `0x000cdc00`), donne directement les 8
pixels de la ligne, de gauche à droite : un chiffre hexa = un pixel, valeur
0-15 = indice dans la palette 16 couleurs (`RED[]`/`GREEN[]`/`BLUE[]` dans
`main.h`, format 0xRR/0xGG/0xBB sur l'octet haut).

Point d'attention pour le portage — **pourquoi le code d'extraction marche
malgré lui** : `drawtile()` ne lit pas les chiffres du littéral, il fait
`tile % 0x10` / `tile /= 0x10` en boucle tant que `tile != 0`, en écrivant
dans un tableau `tiletab[64]` pré-rempli à zéro. Comme un entier ne conserve
aucune trace de ses zéros de poids fort (`0x000cdc00 == 0xcdc00` en mémoire),
la boucle s'arrête dès qu'il ne reste plus que des zéros de tête à
« consommer » — et comme `tiletab` est déjà à zéro à ces positions, le
résultat reconstruit est identique à une lecture directe des 8 chiffres.
**Ça ne marche que parce que les seuls zéros implicites sont ceux de poids
fort (colonnes de gauche de la tuile)** ; un zéro « au milieu » d'un
littéral (ex. le `00` central de `0x000cdc00`) fait partie de la valeur et
est restitué correctement par la division/modulo. À préserver telle quelle
si on veut un rendu identique — ou à remplacer par une lecture directe des
nibbles (comportement strictement équivalent, plus lisible), à valider avec
toi.

Le commentaire d'origine dans `tiles.h` (« cga encoding, two bits per
pixel ») décrit la variante **PC/CGA** de xrick (`GFXPC`, 2 bits/pixel), pas
la variante **ST** utilisée ici (`GFXST`, 4 bits/pixel = 16 couleurs) : à ne
pas reprendre tel quel dans la doc portée, il induirait en erreur.

Plage documentée dans `tiles.h` (256 tuiles/banque) :

```
0x00-0xFF tuiles intro principale
0x100-0x1FF tuiles intro carte
0x200-0x327 inutilisé
0x328-0x427 tuiles de jeu, page 0
0x428-0x527 tuiles de jeu, page 1
0x527-0x5FF inutilisé
```

---

## 3. Format d'un bloc (`map_blocks[0x100][16]`)

256 blocs, chacun = 16 numéros de tuiles (`int`), organisés en grille **4
colonnes × 4 lignes** (`drawblock` : colonne = `i % 4`, ligne = `i / 4`).
Un bloc = 32×32 pixels « logiques » (avant mise à l'échelle), soit 4 tuiles
de 8×8 dans chaque sens.

---

## 4. Format de la carte (`map_bnums[0x1FD8]`)

8152 entiers = indices de blocs (0-255, cohérent avec les 256 entrées de
`map_blocks`). Curiosité à noter : plusieurs lignes contiennent des valeurs
alternatives **commentées en fin de ligne** (ex. ligne 44 :
`0x54, 0x49, 0x48, 0x71, 0x54, 0x6e, 0x6e, 0x6e, //0x54, 0x49, 0x48, 0x71, 0x54, 0x53, 0x55, 0x54,`)
— un historique d'édition manuel du niveau, à conserver tel quel (en
commentaire) dans la version portée plutôt qu'à supprimer.

---

## 5. Système de coordonnées

Trois échelles imbriquées, du pixel écran vers l'index de carte :

```
pixel écran = pixel logique × ScaleFactor        (ScaleFactor = 2, const)
pixel logique tuile = colonne/ligne dans une tuile × 1   (tuile = 8×8)
pixel logique bloc  = tuile × 8                    (bloc = 4×4 tuiles = 32×32)
```

- `drawtile(x, y, NbBank, NbTile, ecran)` : `x`, `y` sont déjà en pixels
  logiques (pas en tuiles) ; la fonction les multiplie par `ScaleFactor`
  pour obtenir la position écran finale.
- `drawblock(x, y, NbBank, NbBlock, ecran)` : `x`, `y` sont en **unités de
  bloc** ; convertis en pixels logiques via `×32` avant l'appel à
  `drawtile`.
- `drawmap(ecran)` : parcourt une fenêtre fixe de **8 colonnes × 128 lignes**
  de blocs (`i` de 0 à 1023, colonne = `i % 8`, ligne = `i / 8`), avec un
  décalage horizontal `FCol` (colonne de départ) et un décalage vertical
  `FRow` appliqué **sur l'index dans `map_bnums`** (`map_bnums[i + FRow*8]`,
  pas de multiplication supplémentaire par une largeur de carte — la carte
  est stockée comme un flux plat de « lignes » de 8 blocs).

### Bizarreries du contrôle clavier/souris à préserver telles quelles

Le sujet étant « ne pas modifier le comportement », je liste ce qui *semble*
être des bugs ou du code mort mais que je n'ai **pas corrigé** :

1. **`DisBank` ne dépend pas du paramètre `NbBank` transmis par
   `drawblock`/`drawmap`.** `drawtile` lit directement la globale
   `tiles_data[DisBank][...]` et ignore son propre paramètre `NbBank`. Le
   `1` codé en dur dans l'appel `drawblock(..., 1, ...)` de `drawmap` n'a
   donc aucun effet — c'est bien les touches `1`/`2` (qui modifient
   `DisBank`) qui pilotent l'affichage.
2. **Haut/Bas font défiler `FRow` de ±1, Gauche/Droite font défiler `FRow`
   de ±16** (pas `FCol`). `FCol` est initialisé à `1` et n'est ensuite
   jamais modifié par aucun gestionnaire d'événement : le défilement
   horizontal proprement dit n'existe pas dans les faits.
3. **Le clic gauche (`leftclick`) calcule un index de carte incohérent**
   avec celui utilisé par `drawmap` : `PosBlock = ((X%8)-FCol)*(Y+1)`
   n'intègre pas `FRow` et multiplie au lieu d'additionner ligne/colonne
   (alors que `drawmap` indexe via `i + FRow*8`). Le clic n'incrémente donc
   presque jamais le bloc réellement affiché sous la souris.
4. `cb.bmp` (54 Ko) est listé dans le projet Code::Blocks mais n'est chargé
   par aucun appel (`SDL_LoadBMP` absent) — probablement une icône d'IDE,
   sans effet sur le programme.
5. Le titre de fenêtre (`"Ma super fenêtre SDL !"`) est encodé en
   ISO-8859-1 dans le fichier source (le fichier entier `main.cpp` est en
   ISO-8859, tous les autres en ASCII/CRLF) : à réencoder en UTF-8 pour un
   affichage correct sous Ubuntu, sans changer le texte.

Je peux soit garder ces comportements strictement à l'identique (recopiés
tels quels), soit les garder actifs mais les signaler par un commentaire
`// NOTE: comportement d'origine, non corrigé` à chaque endroit concerné —
dis-moi ce que tu préfères.
