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
- [x] Zones d'orage sans nuage : dans la grille 3D (~50 km autour de la caméra), les orages n'étaient dessinés que par les tours des orages suivis, et le cumulus 2D s'efface quand la convection monte. Toute zone convective hors des tours (multicellule, grande zone de pluie) était du ciel bleu ; la tornade et son nuage-mur flottaient seuls à côté du nuage. La grille dessine maintenant une colonne convective partout où la simulation convecte (base -> sommet de la case, pluie sous la base), cases interpolées entre t et t+1 (test `Convective cells get cloud in the grid`). Coût inchangé sur la vue à 25 km (5,1-6,2 ms). La tornade relit son orage 10 fois par seconde.
- [x] Nuages et montagnes (orographique) : la base et le sommet sont des hauteurs au-dessus du sol dans la simulation ; la carte 2D, la grille 3D (orages, couche stratiforme) les posent maintenant sur le relief (test `Storm base raised by the ground height`). Pas encore vu à l'écran près des montagnes.
- [x] Bords « salés » sur les images fixes : le rendu maison trace en demi-résolution et lisse les silhouettes contre le ciel.
- [ ] Vues du sol, les cumulus proches cachent les tours d'orage lointaines.
- [x] Face sombre du côté opposé au soleil (rendu maison : ombre vers le soleil + ciel SH × couleur de la sky light).
- [x] Tornades dans les nuages (rendu maison) : entonnoir physique qui descend de la base, nuage-mur qui tourne, poussière / embruns, multi-vortex, corde en fin de vie.
- [ ] Tornades : voir un entonnoir suspendu sous un orage à base haute (l'orage de la visite a une base à 300 m).
- [ ] Tornades : à grande vitesse du temps l'acteur avance en temps réel et l'orage en temps simulé (la tornade quitte son orage) ; sur la mer l'acteur est posé sur le fond.
- [x] Supercellules variées (Keraunos) : LP / classique / HP / LT d'après l'humidité de l'air qui nourrit l'orage (`PrecipLoad`, `SupercellType`). Pluie placée dans le repère de l'orage (cœur à l'avant gauche, crochet autour du mésocyclone, entrée d'air sèche à l'avant droit ; HP : la pluie fait le tour, la tornade est dedans ; LP : peu de pluie), miroir dans l'hémisphère sud. Nommés dans les HUD (« Supercellule HP, mature »). Testé hors moteur (grille 3D). Pas encore bien vu à l'écran : du sol à 20 km les cumulus proches cachent tout, et le pilote auto de la chasse est parti dans les montagnes.
- [x] Voir l'orage vivre : stade (en formation / mature / en fin de vie), enclume qui se construit en ~15 min, survit à l'orage puis se dissout (l'orage mort reste comme enclume seule), tour qui maigrit quand il s'effondre ; nuage-mur qui s'abaisse et tourne à mesure que le mésocyclone s'organise, stries de la tour (rendu maison). Testé (`Aeros.Simulation.StormLife`, grille 3D) ; le nuage-mur tournant et les stries ne sont pas encore jugés à l'écran.
- [x] Tornades qui montent en puissance : EF0 -> EF1 -> ... -> pic -> ... -> EF0, une catégorie à la fois (~6 % de la vie chacune), entonnoir cône -> wedge ; le HUD de la chasse dit « se renforce » / « faiblit ». Échelle EF corrigée (décalée d'une catégorie). Vu dans le log en chasse : EF0 -> EF1 -> EF2 -> EF3.
- [ ] Orages qui se scindent (moteur droit / moteur gauche, supercellules anticycloniques) : essayé puis retiré. Les deux moitiés restaient collées : le transport des nuages prend la vitesse de la case d'arrivée, une case vide avance au vent moyen, donc la moitié droite ne dévie pas. Il faudrait que la vitesse d'orage s'étende aux cases voisines avant l'advection.
- [ ] Vérifier à l'écran, de près : nuage-mur tournant, tour striée d'une LP, tornade cachée dans la pluie d'une HP, enclume qui reste après l'orage. Un cadrage de test fiable manque (l'étape 3 de la visite ne vise pas toujours la supercellule).
- [x] Debug des mises à jour : `aeros.DebugUpdates 1` (console) ou `-AerosDebugUpdates`. Il affiche dans les deux modes l'intervalle t -> t+1 affiché et le chemin parcouru, l'avance de la simulation et le dernier échange de la grille 3D.
- [x] Sauts de la tour d'orage : le courant ascendant était la case de 2 km la plus forte de tout l'orage. Il sautait d'une case, et de 10 km et plus quand une cellule du flanc prenait le relais. Maintenant : courant ascendant suivi localement (le plus fort à 6 km de la position prédite), position entre les cases, filtrée, vitesse de la tour (test `Aeros.Simulation.SmoothStormTrack` : écart < 1,5 km par minute, avant > 12 km), puis **interpolation** : les orages sont suivis à chaque état (30 s simulées), l'affichage, un pas derrière la simulation, fait glisser chaque orage de sa position en t vers t+1 puis t+2 (marqueurs, tornades, grille des nuages reconstruite chaque seconde). Mesuré en chasse : 0,1-0,36 km toutes les 5 s. Puis l'horloge d'affichage : elle visait un pas derrière la simulation, butait sur le dernier état pendant son calcul puis sautait d'un pas entier (450 m à x60) ; elle a maintenant sa propre horloge, deux pas derrière, qui accélère ou ralentit doucement (historique de 6 états). Détecteur dans le debug (saut image à image au-delà du mouvement normal, logué Aeros jump) : 0 en chasse x2, 0 à x60 sur les étapes orage / supercellule ; restent quelques sauts de 300-600 m à x60 quand de jeunes orages naissent (leur courant ascendant change vraiment de place).
- [ ] Lueur des éclairs la nuit (rendu maison, 400 cd/m² plafonné) : vue faible sous un orage, à juger avec l'utilisateur.
- [ ] Dessus des enclumes en « courbes de niveau » vu d'avion (couches voxel de 333 m + seuil de glace x2,5 + auto-ombrage).
- [ ] Time-lapse x60 : le détail bulleux glisse (dérive du bruit = vent, pas le mouvement des orages) ; à x600 le flou de mouvement reste.
- [ ] Tour d'orage très bleutée à 25 km (perspective aérienne du moteur) : à régler avec `r.AerosClouds.Haze` si besoin.
- [x] Coût GPU du rendu maison mesuré : 2,8-3,5 ms à `Quality 2` (`-AerosTourExec=profilegpu`).

## Types de nuages (branche `claude/cloud-types`)

Fait (premier jet, à juger à l'écran) :
- [x] Cumulus moins « boule » : contour bosselé par un bruit à chaque hauteur (lobes, tours), sommet en chou-fleur au lieu d'un dôme, cœur plein réduit, bords plus marqués (effet « powder »). Encore un peu granuleux, comme de la ouate ; ombres un peu bleues.
- [x] Genres par couche, chacun dans ses conditions et à son altitude (`Core/AerosCloudTypes.h`, testé) :
  - bas : Cu humilis / mediocris / congestus, St, Sc, Ns ;
  - moyen : Ac, As ;
  - haut : Ci, Cs, Cc, sur une nouvelle humidité de haute troposphère transportée par le vent de 500 hPa ;
  - Cb déjà là (orages).
- [x] Rendu : nappes St / Sc / Ns, As (nappe grise épaisse), Ac (galets), Ci (fibres dans le vent), Cs (voile), Cc (grains).
- [x] Le HUD de la démo technique nomme les nuages au-dessus (« Cumulus mediocris, Altocumulus, Cirrus »).
- [ ] Juger à l'écran les Ci / Cs / Cc et l'As vus du sol (les essais n'ont montré que des cumulus, des Ac, et le gris d'un Ns ou d'un St vus de dedans), et de haut (aucun cadrage d'avion pour les couches).
- [ ] Cumulus : moins de grain en surface, bords ensoleillés plus nets, ombres moins bleues.
- [ ] Le soleil ne voit pas encore les nouvelles couches dans la simulation (un As épais devrait couper le chauffage du sol et les cumulus).

Pas encore fait :
- [ ] Espèces : lenticularis (onde orographique : Ac / Sc / Cc lenticulaires sous le vent du relief, air stable et vent fort), castellanus (tourelles sur base commune, Ac / Sc / Ci), floccus (flocons à base déchiquetée, virga), fibratus / uncinus (crochets : fort cisaillement), spissatus (Ci épais, souvent reste d'enclume), fractus, nebulosus, volutus (rouleau), calvus / capillatus (stades du Cb).
- [ ] Nuages annexes : pileus (coiffe lisse sur un Cu con / Cb), velum, pannus (lambeaux sous Ns / Cb), virga, cavum (trou dans un Ac), asperitas, fluctus (Kelvin-Helmholtz), murus avec flumen et cauda, tuba.
- [ ] Orographique : mur de foehn (bloqué sur la crête au vent), nuage en bannière (sous le vent d'un pic), mer de nuages (St / Sc piégé sous une inversion dans une vallée).
- [ ] Anthropiques : traînées de condensation (persistantes si < -40 °C et sursaturé / glace), panaches de centrales.
- [ ] Variétés : translucidus / opacus / perlucidus, undulatus, radiatus, duplicatus, intortus, vertebratus, lacunosus.
- [ ] Optique : halo à 22° et parhélies dans les Cs, irisations (Ac, Cc, pileus).
- [ ] Nuages nacrés (stratosphère, hiver polaire, < -78 °C) et noctulescents (mésosphère, été 50-70°, la nuit).
- [ ] Nuage de condensation autour d'un avion transsonique.

## Radar et interface

- [ ] Mode libre : radar plus zoomé par défaut.
- [ ] Zoomer / dézoomer le radar en jeu (les deux modes), sans touche de fonction.
- [ ] Afficher le radar en grand (plein écran ou grand panneau).
- [ ] Mode « satellite » du radar : les nuages vus d'en haut, leur hauteur en dégradé de gris (bas = gris foncé, sommets hauts = blanc).

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
