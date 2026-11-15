# TODO Aeros

Cocher `[x]` quand c'est fait, ~~barrer~~ ce qu'on abandonne, ajouter des lignes au fil de l'eau.
Branche de travail actuelle : `claude/voxel-clouds`.

## Nuages et orages

- [ ] Les nuages sautent à chaque minute de l'horloge. Piste : la simulation ne recalcule les orages qu'une fois par minute simulée (`StormTimer`), mais la grille voxel est reconstruite à chaque état (~30 s), et l'advection repart de zéro.
- [ ] Bords « salés » sur les images fixes (pixels du tracé au quart de résolution) : plus de scintillement, mais le grain se voit.
- [ ] Vues du sol, les cumulus proches cachent les tours d'orage lointaines.
- [ ] Il manque une face vraiment sombre du côté opposé au soleil.
- [ ] Nuage-mur des tornades : encore un disque en maillage, devrait venir des nuages volumétriques.
- [ ] Lueur des éclairs dans les nuages la nuit : réduite (x6 → x2), à vérifier en image.
- [ ] Re-mesurer le coût GPU, éditeur seul, sans jeu en parallèle.

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

- [x] Scintillement des nuages (mode 0, bruits lissés, cœur plein, plus de miettes).
- [x] Enclumes : plus de blocs superposés, glace fibreuse, relief, enclumes qui divergent et se courbent.
- [x] Orages qui glissent entre deux états (grille advectée, carte de forme en fondu), cumulus sans « pop ».
- [x] Pluie volumétrique à la place du cylindre blanc.
- [x] Éclairs : fréquence réaliste, plus de rafales, matériau additif (plus d'éclair noir), plafond d'affichage.
- [x] Paysage procédural (collines, montagnes, lacs, mer).
