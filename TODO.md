# TODO Aeros

Cocher `[x]` quand c'est fait, ~~barrer~~ ce qu'on abandonne, ajouter des lignes au fil de l'eau.
Branche de travail actuelle : `claude/clouds-v2` (rendu maison des nuages).

## Nuages et orages

- [x] Les nuages sautent à chaque minute de l'horloge. Cause : la simulation ne recalcule les orages qu'une fois par minute simulée (`StormTimer`), mais la grille voxel est reconstruite à chaque état (~30 s) avec ces positions périmées. Corrigé : le snapshot porte `StormSimTime`, la grille avance chaque orage de vitesse × âge (test `Stale storm positions…`). Pas encore revu à l'écran.
- [x] EF4-EF5 : un gros wedge (`Aeros::Funnel::WedgeMargin`, pied ≈ 1,5 rayon de cœur), les sous-tourbillons tournent à sa surface près du sol (avant : 5 cordes fines autour d'un voile presque vide). Testé hors moteur, pas encore vu à l'écran (les essais auto finissaient dans la pluie ou derrière une colline).
- [ ] Tornade sous un orage à base haute (entonnoir suspendu), embruns de près.
- [x] Orage et tornade qui se déplacent d'un coup : toute lecture des orages (`GetStorms`, `GetNearestStorm`, tornades, naissance des tornades, grille) les avance de vitesse × âge depuis leur suivi (`FAerosSnapshot::StormAt`, testé). Reste un petit saut (~300 m par minute simulée) : la vitesse estimée au suivi n'est pas exacte.
- [x] Mode chasse, nuages décalés de plusieurs km : les marqueurs et distances visaient `Location` (centre de l'écho, 17 km de la tour dans le cas mesuré). Tout vise maintenant `UpdraftLocation` (la tour). Log de test : `-AerosChaseLogStorm`.
- [ ] Du sol en mode chasse, la tour de la supercellule à 20 km se lit mal : elle est bien là (densité 1,0 dans la grille au bon endroit, `-AerosChaseAimStorm` pour la viser), mais pâle et bleutée, peu contrastée, à moitié cachée par les cumulus proches.
- [x] Tornade hors des nuages : ancrée à 3,4 km de la tour (souvent hors de la tour), traînant v × 4 s derrière son ancrage (3,6 km à x60), perdant son parent quand l'orage changeait de numéro. Maintenant à ~1 km derrière / à droite du courant ascendant (`AAerosTornado::AnchorFor`), portée par la vitesse de l'orage, et elle adopte l'orage le plus proche (15 km). Plus : `Recenter` ne décalait pas la tour suivie (saut après chaque trajet rapide, test), et un état republié à la même heure vidait l'historique de l'affichage (saut de 2 pas).
- [x] Nuages et montagnes (orographique) : la base et le sommet sont des hauteurs au-dessus du sol dans la simulation ; la carte 2D, la grille 3D (orages, couche stratiforme) les posent maintenant sur le relief (test `Storm base raised by the ground height`). Pas encore vu à l'écran près des montagnes.
- [x] Bords « salés » sur les images fixes : le rendu maison trace en demi-résolution et lisse les silhouettes contre le ciel.
- [ ] Vues du sol, les cumulus proches cachent les tours d'orage lointaines.
- [x] Face sombre du côté opposé au soleil (rendu maison : ombre vers le soleil + ciel SH × couleur de la sky light).
- [x] Tornades dans les nuages (rendu maison) : entonnoir physique qui descend de la base, nuage-mur qui tourne, poussière / embruns, multi-vortex, corde en fin de vie.
- [ ] Tornades : voir un entonnoir suspendu sous un orage à base haute (l'orage de la visite a une base à 300 m).
- [ ] Tornades : à grande vitesse du temps l'acteur avance en temps réel et l'orage en temps simulé (la tornade quitte son orage) ; sur la mer l'acteur est posé sur le fond.
- [ ] Supercellules variées (peu de pluie / classique / beaucoup de pluie, tours striées), tornades enveloppées de pluie : projet suivant. Avec, demandé en jouant : **voir l'orage vivre**, à l'endroit et à l'envers. Un cumulus qui monte et devient un orage, puis une supercellule ; on commence à voir les nuages tourner (base et nuage-mur qui tournent, tour striée) ; une tornade descend lentement et s'intensifie progressivement ; puis la même chose en sens inverse (la tornade remonte en corde, la rotation faiblit, l'orage s'effondre en pluie, l'enclume se dissout). Aujourd'hui les étapes se voient mal : la grille 3D dessine l'orage d'après son état, sans transition de croissance.
- [x] Debug des mises à jour : `aeros.DebugUpdates 1` (console) ou `-AerosDebugUpdates`. Il affiche dans les deux modes l'intervalle t -> t+1 affiché et le chemin parcouru, l'avance de la simulation et le dernier échange de la grille 3D.
- [x] Sauts de la tour d'orage : le courant ascendant était la case de 2 km la plus forte de tout l'orage. Il sautait d'une case, et de 10 km et plus quand une cellule du flanc prenait le relais. Maintenant : courant ascendant suivi localement (le plus fort à 6 km de la position prédite), position entre les cases, filtrée, vitesse de la tour (test `Aeros.Simulation.SmoothStormTrack` : écart < 1,5 km par minute, avant > 12 km), puis **interpolation** : les orages sont suivis à chaque état (30 s simulées), l'affichage, un pas derrière la simulation, fait glisser chaque orage de sa position en t vers t+1 puis t+2 (marqueurs, tornades, grille des nuages reconstruite chaque seconde). Mesuré en chasse : 0,1-0,36 km toutes les 5 s. Puis l'horloge d'affichage : elle visait un pas derrière la simulation, butait sur le dernier état pendant son calcul puis sautait d'un pas entier (450 m à x60) ; elle a maintenant sa propre horloge, deux pas derrière, qui accélère ou ralentit doucement (historique de 6 états). Détecteur dans le debug (saut image à image au-delà du mouvement normal, logué Aeros jump) : 0 en chasse x2, 0 à x60 sur les étapes orage / supercellule ; restent quelques sauts de 300-600 m à x60 quand de jeunes orages naissent (leur courant ascendant change vraiment de place).
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
