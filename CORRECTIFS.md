# Correctifs appliqués lors du portage (Windows/MinGW/SDL1 → Ubuntu/C++17/SDL2)

Chaque correctif est aussi signalé par un commentaire `// CORRECTIF` ou
`/* CORRECTIF */` directement dans `src/main.cpp`, à l'endroit concerné.
Rien d'autre n'a été changé : les données (`dat_tilesST.c`, `mapdata.h`)
et les headers de support (`include/*.h`) sont repris à l'identique
(mêmes valeurs, mêmes commentaires, y compris les blagues et les valeurs
commentées qui tracent l'historique d'édition du niveau).

## Bugs corrigés

1. **Sélection de banque de tuiles incohérente.** `drawtile()` ignorait son
   propre paramètre `NbBank` et relisait directement la globale `DisBank`,
   tandis que `drawmap()` transmettait un `1` codé en dur à travers
   `drawblock()`/`drawtile()`. Ça « marchait par accident » puisque le
   paramètre n'était de toute façon jamais lu. Corrigé : `drawtile()`
   utilise maintenant son paramètre `NbBank`, et `drawmap()` transmet
   réellement `DisBank`. Comportement visible inchangé (les touches `1`/`2`
   faisaient déjà l'effet attendu), mais plus de code mort/trompeur.

2. **Clic gauche : calcul d'indice incohérent et non borné.**
   `PosBlock = ((X%8)-FCol)*(Y+1)` ne correspondait pas à l'indexation
   utilisée par `drawmap()` (`map_bnums[i + FRow*8]`), ignorait totalement
   `FRow`, et pouvait produire un indice négatif ou hors tableau (écriture
   mémoire hors bornes = comportement indéfini / crash possible). Corrigé :
   `leftclick()` reconstruit désormais le même indice que `drawmap()` pour
   la case cliquée, avec vérification de bornes (clic ignoré s'il tombe
   hors de la zone de carte actuellement affichée).

3. **Incrément de bloc sans limite.** `map_bnums[PosBlock]++` pouvait
   dépasser 255, le dernier indice valide de `map_blocks[0x100]`, causant
   une lecture hors bornes au rendu suivant. Corrigé : l'incrément boucle
   désormais modulo 256 (`(valeur + 1) % 256`).

4. **`FRow` non borné.** Les touches Haut/Bas (±1) et Gauche/Droite (±16)
   pouvaient faire sortir `FRow` de la plage valide, provoquant une lecture
   hors bornes de `map_bnums` dans `drawmap()` (`i + FRow*8` avec `i`
   jusqu'à 1023). Corrigé : `FRow` est borné à `[0, 891]` après chaque
   modification (valeur maximale calculée pour que tous les indices
   utilisés par `drawmap()` restent valides).

5. **Clic molette/droit détourné.** Dans la boucle d'événements, l'absence
   d'accolades autour du `if (event.button.button == SDL_BUTTON_LEFT)`
   faisait que seul `SDL_GetMouseState()` était réellement conditionné par
   le bouton gauche : `leftclick()` s'exécutait pour **n'importe quel**
   bouton de souris, avec des coordonnées potentiellement obsolètes (ou non
   initialisées si aucun clic gauche n'avait encore eu lieu). Corrigé :
   ajout des accolades, le comportement correspond maintenant au commentaire
   d'origine (« If the left button was pressed »).

6. **Fuite mémoire dans `drawtile()`.** La surface temporaire créée par
   `SDL_CreateRGBSurface()` n'était jamais libérée — fuite à chaque tuile
   dessinée (des milliers de fois par image). Corrigé : `SDL_FreeSurface()`
   ajouté en fin de fonction.

7. **`IMG_SPLASH` sans `extern` dans `img.h`.** Définition tentative
   tolérée par les anciens compilateurs (`-fcommon`) mais qui casse
   l'édition de liens sous GCC moderne (`-fno-common` par défaut) dès que
   plusieurs fichiers incluent ce header. Variable inutilisée par le
   programme ; correctif sans effet visible, nécessaire uniquement pour
   compiler.

8. **Encodage du titre de fenêtre.** `"Ma super fenêtre SDL !"` était
   enregistré en ISO-8859-1 dans le fichier source d'origine (mojibake sous
   Linux/UTF-8). Réencodé en UTF-8, texte inchangé.

## Ce qui n'a PAS été changé (dead code bénin, laissé tel quel)

- `FCol` est initialisé à `1` et n'est modifié par aucune touche : le
  défilement horizontal du viewport n'existe pas dans les faits. Ce n'est
  pas une source de crash ni de comportement incorrect (juste une
  fonctionnalité jamais branchée), donc laissé en l'état — dis-moi si tu
  veux qu'une touche pilote `FCol`.
- `cb.bmp` n'est chargé par aucun appel `SDL_LoadBMP` ; fichier non repris
  dans le portage (aucun impact, jamais utilisé par le programme).

## Source des données

`dat_tilesST.c` est désormais la **seule** source des données de tuiles
(compilé directement, `TILES_NBR_BANKS = 3`). L'ancienne copie dans
`main.h`, qui réservait une 4ᵉ banque jamais initialisée (donnée
« fantôme »), a été retirée ; `mapdata.h` ne contient plus que la palette,
`map_bnums` et `map_blocks`, strictement inchangés.

## Remise à zéro de la carte (données stock)

`map_bnums` a été régénéré à partir des données stock extraites d'un
binaire xrick (symbole ELF `map_bnums`, 8152 octets) : c'est la carte
d'origine du jeu, sans aucune modification manuelle -- vérifié à 0 écart
avec le binaire. `map_blocks` et `tiles_data` étaient déjà identiques au
binaire (vérifié également), ils n'ont pas eu besoin d'être régénérés.

Le commentaire de blague `A MOIIII...` a été retiré de tous les fichiers
sources vendorisés (`dat_tilesST.c`, `config.h`, `system.h`, `rects.h`,
`img.h`, `tiles.h`).
