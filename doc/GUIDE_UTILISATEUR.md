# Guide de l'éditeur de niveaux Rick Dangerous — pour les non-informaticiens

Ce guide explique, en français et sans jargon technique, comment utiliser
l'éditeur de niveaux pour **modifier les niveaux du jeu Rick Dangerous**.
Vous n'avez pas besoin de savoir programmer : tout se fait avec la souris
et quelques boutons.

Ce guide est volontairement simple. Si vous voulez les détails techniques,
lisez `EDITEUR.md` (très technique) ou `CORRECTIFS.md`.

---

## 1. Ce que vous allez apprendre

- **Lancer l'éditeur** et charger un niveau.
- **Ce que sont une tuile, un bloc et une zone de déclenchement** — les
  3 briques de base du jeu.
- **Poser des blocs** pour construire les murs, les sols, les escaliers…
- **Placer des ennemis et des pièges** (appelés « sprites »).
- **Comprendre et déplacer les zones de déclenchement** (les zones
  invisibles qui réagissent quand Rick passe dedans).
- **Relier les écrans** entre eux (les « connections »).
- **Sauvegarder votre travail.**
- **Appliquer vos niveaux au vrai jeu** (on dit « patcher » le jeu).

---

## 2. Lancer l'éditeur

Lancez le programme (par exemple depuis le terminal, dans le dossier du
projet) :

```
./build/rickeditor
```

---



## 3. Les 3 briques de base : tuile, bloc, submap

Le jeu est construit comme un **puzzle à plusieurs étages**.

### La tuile (le plus petit morceau)

Une **tuile** est un petit carré d'image de 8×8 pixels : un morceau de mur,
un morceau de sol, un bout d'escalier, une pointe, etc. C'est la plus
petite unité visuelle du jeu. Il y a 256 tuiles par « banque », et il
existe 3 banques (0, 1 et 2).

Chaque tuile possède aussi **8 cases à cocher** qui définissent son
comportement dans le jeu. Les plus importantes :

| Case | Effet dans le jeu |
|---|---|
| **Solid** | Bloquant : Rick ne peut pas la traverser (un mur, un sol, une porte fermée). |
| **Lethal** | Mortelle : tue Rick si elle le touche (pointes, lave…). |
| **Climb** | Escalier : Rick peut grimper dessus avec ↑. |
| **WayUp** | Palier : le sol sur lequel Rick peut marcher. |
| **SuperPad** | Tremplin : propulse Rick en l'air. |
| **Fgnd** | Premier plan : affiché PAR-DESSUS Rick (feuillage, grilles…). |

> ⚠️ Important : quand vous modifiez le comportement d'une tuile, cela
> s'applique **partout** sur la carte où cette tuile est utilisée. On ne
> peut pas en faire une version « spéciale » juste à un endroit.

### Le bloc (la pièce du puzzle que vous posez)

Un **bloc** est un carré de 4×4 tuiles (soit 32×32 pixels). C'est LA pièce
que vous placez sur la carte, un peu comme on pose des blocs de
construction. Il y a 256 blocs prédéfinis.

Vous ne placez presque jamais une seule tuile : vous **placez des blocs**
déjà assemblés. C'est beaucoup plus rapide et c'est le format du jeu
d'origine. Si besoin, vous pouvez changer quelles tuiles composent un bloc
grâce à l'« éditeur de blocs » (voir plus bas).

### La submap (l'écran = un morceau du niveau)

Le jeu ne défile **pas** latéralement à proprement parler : un niveau est
une **chaîne d'écrans fixes**, appelés **submaps**, empilés pour former
toute la hauteur du niveau. Rick **sort toujours par les côtés** de
l'écran pour passer au suivant — la direction **Up / Down** d'un lien
indique seulement si l'écran voisin est plus haut ou plus bas dans la
pile. Chaque submap est une « fenêtre » qui pointe sur une portion de
cette colonne.

---

## 4. Se repérer dans la fenêtre

En haut de la fenêtre de l'éditeur, vous avez une barre d'outils. Les
éléments principaux :

- **Submap / Block / Sprite** : trois boutons pour choisir « le mode » de
  travail (voir ci-dessous).
- **Grille** : une case à cocher (touche `G`) pour afficher la grille des
  blocs à l'écran.
- Les boutons **zoom** (`+`/`-`) et les **flèches** pour se déplacer dans
  le niveau.
- La **position** du curseur en bas de la carte.

### Les 3 modes de travail

| Mode | À quoi il sert | Ce qu'il affiche |
|---|---|---|
| **Submap** | Examiner et relier les écrans | La fenêtre **Screen Connections** |
| **Block** | Construire : poser des murs, sols… | La fenêtre **Block Palette** |
| **Sprite** | Placer ennemis, pièges, objets | La fenêtre **Sprite Tools** |

Le zoom et le déplacement fonctionnent dans tous les modes.

**Se déplacer dans le niveau** :
- **Molette de la souris** : zoom (centré sur le curseur).
- **Touche `+` / `-`** : zoom (centré sur l'écran).
- **Touches flèches Haut/Bas** : se déplacer de fine en fine.
- **Flèches Gauche/Droite, PageHaut/PageBas** : grands sauts.
- **Molette du milieu (clic maintenu)** : faire glisser la carte.

---

## 5. Construire un niveau (mode Block)

C'est le mode le plus utilisé : c'est là qu'on pose les murs et les sols.

### La palette de blocs

La fenêtre **Block Palette** (à droite) montre tous les blocs disponibles.
Cliquez sur un bloc pour le **sélectionner** — c'est le bloc que vous allez
poser.

### Poser un bloc

1. Choisissez le mode **Block** (bouton en haut).
2. Cliquez sur un bloc dans la palette pour le sélectionner.
3. **Clic gauche** sur la carte pour le poser.
   - **Clic gauche maintenu** : vous peignez plusieurs blocs d'affilée (à
     la façon d'un pinceau).

### Copier un bloc déjà sur la carte

Plutôt que de chercher dans la palette, vous pouvez « prélever » un bloc
déjà posé :

- **Clic droit** (maintenu pour continuer) sur un bloc de la carte : il
  devient le bloc sélectionné (comme une pipette).

### Effacer / sélectionner une zone

- **Maj + glisser** sur la carte : trace un rectangle de sélection.
- **Suppr / Retour arrière** : efface la sélection (la remet à « vide »).
- **F** (ou bouton **Fill selection**) : remplit la sélection avec le bloc
  choisi.
- **Échap** : annule la sélection.

### Astuce : changer de banque de graphismes

- Touches **`1`** et **`2`** : basculer entre la banque de tuiles 1 et 2
  (deux styles de décors différents).

> La banque 0 contient la police de caractères et les décors des écrans
> d'animation ; elle est cachée de la palette car aucun bloc du jeu ne
> l'utilise en jeu.

---

## 6. Personnaliser les blocs (Éditeur de blocs)

Pour changer **quelles tuiles forment un bloc** :

1. Menu **Tools → Block Editor**.
2. À gauche : la grille des 256 blocs. Cliquez-en un.
3. À droite : un **carré 4×4** montre la composition du bloc.
   - Cliquez une case du carré, puis cliquez une tuile dans le sélecteur
     pour la mettre à cette place (l'éditeur avance tout seul à la case
     suivante pour composer rapidement).
   - **Clear block** : vide tout le bloc (tout remis à « rien »).
   - **Swap with…** : échange la composition complète de deux blocs.
   - **Copy to…** : copie la composition d'un bloc vers un autre.

Tout se met à jour en direct sur la carte : pas besoin de recharger.

---

## 7. Personnaliser les tuiles (Éditeur de tuiles)

Pour modifier **l'image** d'une tuile ou **son comportement** :

1. Menu **Tools → Tile Editor**.
2. À gauche : les 256 tuiles de la banque choisie. Cliquez-en une.
3. À droite :
   - Une **aperçu agrandi** de la tuile.
   - **Import from image…** : chargez une image pour remplacer le dessin de
     la tuile (elle est automatiquement adaptée au format du jeu).
   - Les **cases à cocher de comportement** vues plus haut (Solid, Lethal,
     Climb, etc.).

### Importer plusieurs tuiles d'un coup

Dans la section **Batch import…** :
1. Choisissez une **tuile de départ** (le point de départ de la rangée).
2. Choisissez une image découpée en petits carrés 8×8.
3. Chaque carré devient une tuile, l'une après l'autre.

Les cases vides de l'image sont ignorées automatiquement.

---

## 8. Placer des ennemis et des pièges (mode Sprite)

Les « sprites » regroupent les **ennemis**, **pièges**, **objets** et
**trésors** du jeu.

### Placer un sprite

1. Choisissez le mode **Sprite** (bouton en haut).
2. Dans la fenêtre **Sprite Tools**, activez **Sprite placement mode**.
3. Choisissez le **type** d'entité dans la liste (les types déjà utilisés
   ailleurs sont proposés en raccourci).
4. **Clic gauche** sur la carte pour placer l'entité.
   **Clic droit** sur une entité pour la retirer.

Chaque entité placée affiche **sa vraie première image** (l'ennemi tel
qu'il apparaît dans le jeu), ou un petit point de couleur si ce type n'a
pas de dessin distinct.

### Modifier une entité placée

Dans Sprite Tools, la liste des sprites du sous-écran courant vous montre,
pour chacun :

- **Row / Column** : sa position (ligne / colonne).
- **Entity type** : son type.
- **flags** : pour les pièges, les cases qui choisissent ce qui les
  déclenche (Rick, Cane, Bullet, Bomb) et comment ils réagissent
  (Once, LethalWake, LethalLoop, StopsRick) — voir plus bas.
- **trigger →** : sa **zone de déclenchement** (voir juste après).

---

## 9. Les zones de déclenchement (« trigger boxes »)

Une **zone de déclenchement** est une **zone invisible** qui **déclenche**
un sprite quand quelque chose s'y produit. C'est le « capteur » d'un
piège ou d'un ennemi.

### À quoi servent les zones de déclenchement ?

Elles servent à dire **quand et où** un sprite se met en action. Le plus
souvent, la zone sert de **capteur de présence** : quand **Rick entre
dedans**, le piège se déclenche. Pour d'autres sprites, c'est encore
différent :

- **Les pièges** (pointes, flèches, grilles, rochers…) : la zone détecte
  l'arrivée de Rick (ou d'un projectile, ou d'une bombe) et active le
  piège à ce moment-là.
- **Certains ennemis endormis** : la zone « réveille » l'ennemi quand Rick
  s'approche (ils attendent dans leur coin, puis se mettent à agir).
- **Les murs explosifs / blocs à bombe** : la zone réagit quand une bombe
  explose à proximité — le bloc disparaît, tombe, ou devient mortel un
  court instant.
- **Le déclencheur sonore** : un sprite **invisible**, sans collision, qui
  sert uniquement à **jouer un bruit** quand Rick traverse sa petite zone
  (un cri, un piège qui grince…).

Bien comprendre cela évite beaucoup de confusion : **le point de
déclenchement n'est pas l'endroit où le sprite est dessiné**, c'est
**l'endroit où sa zone invisible se trouve**.

Exemple : un **piège à flèches** monté sur un mur. Sa zone de
déclenchement peut être posée **à côté du piège**, sur la plaque du sol
que Rick doit traverser pour le déclencher. Le dessin du piège (fixé au
mur) ne bouge pas ; seule la zone invisible se déplace avec les réglages
ci-dessous.

### Les cases « flags » : ce qui déclenche, et comment le sprite réagit

Dans Sprite Tools, chaque piège (type « déclenché ») possède une rangée
de cases à cocher **flags →**. Elles servent à **choisir le
déclenchement** et **l'effet** :

**Le déclenchement (par quoi le piège se déclenche) :**

| Case | Effet |
|---|---|
| **Rick** | Le piège se déclenche quand **Rick marche** dans sa zone. (Le plus courant.) |
| **Cane** | Le piège se déclenche quand Rick **frappe avec sa canne** (touche FIRE + direction) dans la zone. |
| **Bullet** | Le piège se déclenche quand un **projectile** touche la zone. |
| **Bomb** | Le piège se déclenche quand une **bombe/explosion** touche la zone. |

Au moins **une** de ces quatre cases doit être cochée, sinon le piège
**ne se déclenche jamais** (il reste inerte).

**L'effet (ce qui se passe une fois déclenché) :**

| Case | Effet |
|---|---|
| **Once** | Le piège ne joue **qu'une seule fois**, puis disparaît définitivement (au lieu de recommencer en boucle). |
| **LethalWake** | Le piège est **mortel dès son réveil** (ex. une grille de pointes qui surgit et tue au contact). |
| **LethalLoop** | Le piège devient **mortel à chaque redémarrage de sa boucle** de mouvement. |
| **StopsRick** | Le piège **bloque physiquement Rick** comme un mur plein (ex. un bloc mobile qui écrase). |

> 💡 Les pièges placés par défaut sont réglés sur **Rick** avec la zone sur
> la case où vous cliquez — le « ça se déclenche quand Rick passe dessus »
> du cas le plus fréquent. Vous pouvez ensuite cocher une autre combinaison
> (ex. un mur explosif réglé sur **Bomb** uniquement, pour qu'il n'explose
> que si vous le faites sauter).

### Voir les zones de déclenchement

- Menu **View → Trigger boxes** (cocher) : affiche en surimpression la
  **zone de déclenchement** de chaque sprite concerné, sous forme de
  **rectangle** plus ou moins grand selon le type.

Chaque type a une zone de taille différente : une grille a une grosse zone
large et plate, un rocher écrasant une petite zone 3×3, un piège lanceur
une zone large de toute la largeur du passage… La taille de la zone est
propre à chaque type de sprite (elle définit jusqu'où Rick doit s'avancer
pour le déclencher).

### Déplacer la zone de déclenchement

Dans Sprite Tools, sous l'entité, la ligne **trigger →** contient deux
valeurs (une **colonne** et un **décalage de ligne**). En les modifiant,
vous déplacez la **zone** sans bouger le dessin du sprite. Un **petit
réticule rouge** est dessiné sur la carte, relié au sprite, chaque fois que
la zone n'est pas exactement sous le sprite — pratique pour vérifier d'un
coup d'œil où se trouve réellement le capteur.

> ⚠️ Ces réglages « trigger → » ne sont **affichés que pour les types de
> sprites qui s'en servent réellement** (les pièges et ennemis déclenchés
> par zone). Pour les autres types, le jeu ignore complètement cette valeur
> : l'éditeur ne l'affiche donc pas, pour éviter de croire qu'elle a un
> effet alors qu'elle n'en a pas.

> 💡 Conseil : vérifiez toujours où se trouve la zone de déclenchement de
> vos pièges. Un piège qui ne se déclenche pas, c'est souvent une zone mal
> placée ou trop loin de l'endroit où passe Rick.

---

## 10. Relier les écrans (Screen Connections / mode Submap)

En mode **Submap**, la fenêtre **Screen Connections** montre comment les
écrans (submaps) sont reliés entre eux et permet de construire le
**chemin** du niveau.

### Comment se déplace-t-on entre les écrans ?

Le jeu fonctionne par **écrans fixes** : chaque écran est une longue
bande horizontale de la carte (une submap). Rick **sort toujours par les
côtés** (à gauche ou à droite) de l'écran pour passer au suivant — il n'y
a jamais de défilement latéral à proprement parler : on passe d'un écran
au voisin.

Les écrans sont **empilés** les uns par-dessus les autres pour former
toute la hauteur du niveau. Quand Rick quitte un écran pour aller vers
l'écran plus haut ou plus bas, c'est là qu'intervient la **direction**
du lien.

- Chaque lien relie un écran de départ à un écran cible, et dit :
  - **la direction** — **Up** ou **Down** — qui indique seulement si
    l'écran connecté se trouve **plus haut** ou **plus bas** dans la pile
    des écrans (le passage lui-même se fait toujours sur le côté) ;
  - la **ligne de départ** (la ligne de l'écran de départ où débute le
    passage) ;
  - **l'écran cible** et la **ligne d'arrivée** (là où Rick réapparaît
    dans l'écran suivant).
- **Start row** : la ligne du niveau où commence cet écran.
- **+ Add link** / **X** : ajouter / retirer un lien.

### Règles importantes (limites du jeu d'origine)

- Il y a **au maximum 47 écrans** et **153 liens au total** pour tout le
  niveau. Le jeu d'origine est déjà plein : pour **ajouter** un lien, il
  faut d'abord **en retirer** un autre.
- On ne peut pas « supprimer » un écran au sens propre : on peut seulement
  le **déconnecter** (le rendre inaccessible).
- Un lien ne peut pas être trop loin de l'écran qui le contient (au-delà
  de 255 lignes). L'éditeur refuse l'opération avec un message clair si
  c'est le cas.

Le bouton **Check for / fix misaligned block run** (dans le mode Submap)
répare automatiquement un décalage de blocs présent dans les derniers
écrans du jeu d'origine, sans changer le comportement en jeu.

---

## 11. La position de départ de chaque niveau

Le jeu a plusieurs niveaux (Amazonie, Égypte, château…), chacun avec sa
**position de départ** pour Rick.

- Menu **View → Map start positions** (cocher) : affiche sur la carte
  l'endroit où Rick apparaît au début de chaque niveau, sous forme de
  petit marqueur au-dessus des sprites.

---

## 12. Modifier les textes d'animation (Éditeur de texte)

Entre deux niveaux, le jeu affiche des écrans de texte (« SOUTH AMERICA
1945 », « EGYPT, SOMETIMES LATER »…).

- Menu **Tools → Text Editor**.
- À gauche : les 5 textes. Cliquez-en un.
- À droite : les lignes de texte, modifiables.
  - **Blank line after** : ajoute une ligne vide après la ligne courante.
  - **Up / Down / X** : déplacer ou supprimer une ligne.
  - **Add line** : ajouter une ligne.
  - **Reset to stock** : revenir au texte d'origine.
- En bas : un **aperçu en direct** qui montre exactement à quoi ressemblera
  le texte dans le jeu (avec la vraie police de caractères).

Limite : chaque texte doit rester **dans la taille prévue** par le jeu.
Si votre texte est trop long, l'éditeur refusera de l'appliquer (plutôt
que de casser le jeu).

---

## 13. Sauvegarder votre travail

### Le fichier `.map` (votre « brouillon »)

- Menu **File → Save** ou **Save As…** : enregistre TOUT votre travail
  (les blocs posés, les ennemis, les zones, les textes, les images) dans
  un fichier `.map`.
- Menu **File → Open…** : recharge un fichier `.map` pour continuer.

C'est le format de travail de l'éditeur : c'est **ça** que vous devez
sauvegarder régulièrement.

> Une **étoile `*`** à côté du nom du fichier signifie qu'il y a des
> changements non sauvegardés. Sauvegardez avant de quitter !

---

## 14. Appliquer vos niveaux au vrai jeu (« patcher »)

Une fois votre niveau terminé et sauvegardé, il faut **l'injecter** dans
un vrai fichier du jeu pour pouvoir y jouer. C'est ce qu'on appelle
**patcher**.

### Ce qu'il vous faut

1. Le fichier du jeu (l'exécutable `xrick`), **non compressé** (pas
   « strippé » dans le jargon). L'éditeur a besoin que le fichier contienne
   les noms internes des données.
2. Votre travail sauvegardé dans l'éditeur (facultatif : vous pouvez aussi
   patcher l'état actuel sans sauvegarder).

### Étapes

1. Dans l'éditeur, menu **File → Patch xrick binary…**.
2. Choisissez le fichier du jeu.
3. L'éditeur crée une **copie** du fichier, nommée
   **`<nom du jeu>_patched`**, à côté de l'original. **Votre jeu d'origine
   n'est jamais modifié.**
4. Lancez le fichier `_patched` pour jouer votre niveau.

Tout est pris en charge automatiquement : les blocs, les ennemis, les
zones de déclenchement, les liens entre écrans, le comportement des tuiles,
les images et les textes.

> ✔️ Au cas où : si un niveau ne charge pas, vérifiez que le fichier du jeu
> est bien « non strippé » (l'éditeur affiche un message clair s'il ne
> trouve pas les données attendues plutôt que de corrompre le fichier).

### Importer un jeu existant dans l'éditeur

À l'inverse, le menu **File → Import from xrick binary…** recharge dans
l'éditeur les données (niveaux, sprites, tuiles…) directement depuis un
fichier du jeu — pratique pour récupérer un niveau d'un autre build.

---

## 15. Récapitulatif des touches

| Touche / action | Effet |
|---|---|
| `1` / `2` | Changer de banque de tuiles (style de décor) |
| `S` | Basculer entre mode Block et Sprite |
| `G` | Afficher / masquer la grille |
| `F` | Remplir la sélection avec le bloc choisi |
| `Echap` | Annuler la sélection |
| Molette / `+` / `-` | Zoom |
| Flèches / PgHaut / PgBas | Se déplacer |
| **Mode Block** : clic gauche | Poser un bloc |
| **Mode Block** : clic droit | Prélever un bloc (pipette) |
| **Mode Block** : Maj + glisser | Sélectionner une zone |
| **Mode Sprite** : clic gauche | Placer l'entité choisie |
| **Mode Sprite** : clic droit | Retirer l'entité la plus proche |

---

## 16. Si quelque chose ne marche pas

- **Un piège ne se déclenche pas** → vérifiez sa **zone de déclenchement**
  (View → Trigger boxes) et déplacez-la sur le passage de Rick.
- **Un ennemi placé n'apparaît pas en jeu** → c'était un bug de l'éditeur,
  corrigé : les sprites sont maintenant correctement triés avant d'être
  injectés. Re-patche après avoir rechargé et réenregistré le `.map`.
- **Le jeu refuse de se patcher** → le fichier du jeu doit être « non
  strippé ». Message clair affiché par l'éditeur.
- **Un texte ne passe pas** → il est trop long ; raccourcissez-le ou
  supprimez des lignes.

---

*Pour le détail technique, la liste exacte des comportements de chaque
entité et les formats de fichiers, voir `EDITEUR.md`.*
