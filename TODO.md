# TODO Aeros

Cocher `[x]` quand c'est fait, ~~barrer~~ ce qu'on abandonne, ajouter des lignes au fil de l'eau.
Branche de travail actuelle : `claude/clouds-v2` (rendu maison des nuages).

## Nuages et orages

- [ ] Les nuages sautent à chaque minute de l'horloge. Piste : la simulation ne recalcule les orages qu'une fois par minute simulée (`StormTimer`), mais la grille voxel est reconstruite à chaque état (~30 s), et l'advection repart de zéro.
- [x] Bords « salés » sur les images fixes : le rendu maison trace en demi-résolution et lisse les silhouettes contre le ciel.
- [ ] Vues du sol, les cumulus proches cachent les tours d'orage lointaines.
- [x] Face sombre du côté opposé au soleil (rendu maison : ombre vers le soleil + ciel SH × couleur de la sky light).
- [x] Tornades dans les nuages (rendu maison) : entonnoir physique qui descend de la base, nuage-mur qui tourne, poussière / embruns, multi-vortex, corde en fin de vie.
- [ ] Tornades : voir un entonnoir suspendu sous un orage à base haute (l'orage de la visite a une base à 300 m).
- [ ] Tornades : à grande vitesse du temps l'acteur avance en temps réel et l'orage en temps simulé (la tornade quitte son orage) ; sur la mer l'acteur est posé sur le fond.
- [ ] Supercellules variées (peu de pluie / classique / beaucoup de pluie, tours striées), tornades enveloppées de pluie : projet suivant.
- [ ] Lueur des éclairs la nuit (rendu maison, 400 cd/m² plafonné) : vue faible sous un orage, à juger avec l'utilisateur.
- [ ] Dessus des enclumes en « courbes de niveau » vu d'avion (couches voxel de 333 m + seuil de glace x2,5 + auto-ombrage).
- [ ] Time-lapse x60 : le détail bulleux glisse (dérive du bruit = vent, pas le mouvement des orages) ; à x600 le flou de mouvement reste.
- [ ] Tour d'orage très bleutée à 25 km (perspective aérienne du moteur) : à régler avec `r.AerosClouds.Haze` si besoin.
- [x] Coût GPU du rendu maison mesuré : 2,8-3,5 ms à `Quality 2` (`-AerosTourExec=profilegpu`).

## Autres effets et décor

- [ ] Vague du tsunami peu visible.
- [ ] Effet « Séisme » trop long après un M9 (répliques).
- [ ] Voir de près les lacs, les plages et la mer avec ses vaguelettes (ajouter une étape ou un cadrage à la visite).

## Jamais testé

- [ ] Météo réelle (Open-Meteo, `UAerosLiveWeather`).

## Branches

- [ ] Fusionner `claude/voxel-clouds` dans `claude/fervent-galileo-okll3m` (seulement sur demande).

## Mis de côté

- ~~Migrer la carte et les matériaux vers le MCP + PCG (MeshPartition)~~ : annulé, on garde le paysage C++ et `aeros_setup.py`.

## Fait récemment

- [x] Rendu maison des nuages (`AerosCloudRender`, `r.AerosClouds 1`) : plus de scintillement ni de dither quand les nuages bougent ou avec les éclairs, LOD continu, ombres au sol, 8 éclairs.
- [x] Scintillement des nuages (mode 0, bruits lissés, cœur plein, plus de miettes).
- [x] Enclumes : plus de blocs superposés, glace fibreuse, relief, enclumes qui divergent et se courbent.
- [x] Orages qui glissent entre deux états (grille advectée, carte de forme en fondu), cumulus sans « pop ».
- [x] Pluie volumétrique à la place du cylindre blanc.
- [x] Éclairs : fréquence réaliste, plus de rafales, matériau additif (plus d'éclair noir), plafond d'affichage.
- [x] Paysage procédural (collines, montagnes, lacs, mer).
