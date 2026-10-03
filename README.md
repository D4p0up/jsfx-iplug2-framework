# jsfx-iplug2-framework

Développer un instrument (ou un effet) **en JSFX** et obtenir automatiquement
un **VST3 Windows** et un **VST3 + AU macOS (universel)** grâce à iPlug2 et
GitHub Actions.

Le principe est celui du wrapper [ysfx](https://github.com/JoepVanlier/ysfx),
mais au lieu d'un plugin qui charge n'importe quel JSFX, chaque plugin embarque
**un seul fichier JSFX, chargé en permanence** : il fournit à la fois le
traitement audio/MIDI (`@init`, `@slider`, `@block`, `@sample`, `@serialize`)
et l'interface (`@gfx`).

```
plugins/JsfxSynth/
├── plugin.json        nom, fabricant, codes 4 lettres, type (instrument/effet)
├── JsfxSynth.jsfx     tout le plugin : DSP + GUI
└── data/ (option)     samples, images, .jsfx-inc... embarqués aussi
```

`git push` → la CI produit `JsfxSynth.vst3` (Windows) et `JsfxSynth.vst3` +
`JsfxSynth.component` (macOS), validés par `auval`.

---

## Démarrage

Prérequis : CMake ≥ 3.25, Git, Visual Studio 2022 ou 2026 (Windows) ou Xcode (macOS).

```bash
./scripts/setup.sh          # Windows : .\scripts\setup.ps1
```

Le script ajoute les sous-modules **iPlug2** et **ysfx** (épinglés sur des
commits testés), récupère `dr_libs` (seule dépendance de ysfx utilisée) et
télécharge le SDK VST3. Ensuite :

| | Configurer | Compiler | Résultat |
|---|---|---|---|
| macOS | `cmake --preset macos` | `cmake --build --preset macos` | `build/macos/out/Release/*.vst3`, `*.component` |
| Windows | `cmake --preset windows` | `cmake --build --preset windows` | `build/windows/out/*.vst3` |
| Linux | `cmake --preset linux` | `cmake --build --preset linux` | moteur + tests uniquement (iPlug2 ne gère pas Linux) |

Les plugins sont aussi copiés dans les dossiers système de l'utilisateur
(`~/Library/Audio/Plug-Ins/...`, `%LOCALAPPDATA%\Programs\Common\VST3`) ;
désactivable avec `-DIPLUG_DEPLOY_PLUGINS=OFF`.

Tests du moteur : `ctest --preset macos` (ou `windows` / `linux`) :
`engine_smoke` (chargement, audio, paramètres, état, `@gfx`, menus, Retina,
rechargement) et `engine_stress` (audio + `@gfx` + rappels d'état en
parallèle, avec détection d'interblocage).

## Créer un nouveau plugin

1. Copier `plugins/JsfxSynth` vers `plugins/MonPlugin`.
2. Renommer le `.jsfx` et éditer `plugin.json` :

| Champ | Rôle |
|---|---|
| `name` | nom du plugin et des binaires (lettres, chiffres, `_`) |
| `jsfx` | fichier principal (défaut : `<name>.jsfx`) |
| `type` | `instrument`, `effect` ou `midi-effect` |
| `manufacturer` | nom du fabricant |
| `manufacturer_code` / `plugin_code` | 4 caractères chacun ; **uniques** par plugin. Mettez au moins une majuscule dans `manufacturer_code` (Apple réserve le tout-minuscule) |
| `version` | `x.y.z` |
| `midi_in` / `midi_out` | défauts : MIDI in pour instruments et effets MIDI |
| `vst3_subcategory` | défaut `Instrument\|Synth` ou `Fx` |
| `ui_width` / `ui_height` | optionnel : sinon taille de `@gfx w h`, ou grille de potards |
| `resizable` | `false` par défaut : fenêtre de taille fixe, l'hôte ne peut pas la redimensionner et il n'y a pas de poignée. `true` réactive le redimensionnement |

3. Recompiler. Chaque dossier de `plugins/` contenant un `plugin.json` devient
   un plugin : la CI les construit tous.

> **Piège classique en JSFX :** `@gfx` s'exécute sur un autre thread, en
> même temps que `@sample` (c'est aussi le cas dans REAPER). Une variable
> globale utilisée à la fois dans `@gfx` et dans `@sample`/`@block` (un
> compteur de boucle `i`, une variable temporaire `k`...) sera écrasée par
> l'autre thread et peut produire des `inf`/`NaN` dans l'audio. Préfixez les
> variables de `@gfx` (`g_i`, `g_k`...) ou utilisez des fonctions avec
> `local()`. Le test de stress du dépôt a justement attrapé ce bug dans le
> synthé d'exemple.

Tout le reste vient du JSFX lui-même, lu **à la compilation** par ysfx :

* chaque `sliderN:` devient un paramètre hôte (nom, plage, valeur par défaut,
  courbes `:log=` / `:sqr=`, listes `{a,b,c}`, sliders cachés `-`) ;
* `in_pin:` / `out_pin:` donnent les entrées/sorties audio ;
* `@gfx 720 380` donne la taille de la fenêtre ; sans `@gfx`, une interface
  générique de potards est construite automatiquement ;
* une erreur de syntaxe dans le JSFX **fait échouer la compilation** avec le
  message d'EEL2, au lieu de livrer un plugin muet.

## Boucle de développement

1. Écrire et tester le JSFX dans REAPER (rechargement instantané).
2. Pour tester dans le vrai wrapper sans recompiler à chaque fois :
   `cmake --preset macos-dev` (ou `windows-dev`) puis compiler une fois.
   Ces builds Debug chargent le `.jsfx` **directement depuis `plugins/`** et le
   recompilent à chaque sauvegarde, en conservant l'état (sliders +
   `@serialize`). Une erreur de compilation s'affiche en bandeau rouge dans
   l'interface, l'ancienne version continue de tourner.
3. Les builds de release (presets `macos` / `windows`, et la CI) embarquent le
   JSFX figé dans le binaire.

## Architecture

```
┌──────────────────────── un plugin VST3 / AU (iPlug2) ─────────────────────────┐
│ JsfxPlugin (framework/plugin)            classe iPlug2 unique, générique      │
│   ProcessBlock ──► Engine::Process() ──► ysfx_process_*   (thread audio)      │
│   OnParamChange ─► SetParamValue()  ┐    sliders ⇄ paramètres via atomiques   │
│   OnIdle ◄──────── ConsumeSliderChanges()  slider_automate → automation hôte  │
│   Serialize/UnserializeState ⇄ SaveState/LoadState (sliders + @serialize)     │
│ JsfxGfxControl ─► GfxRunner (thread @gfx) ─► framebuffer LICE ─► texture NanoVG│
│                   gfx_showmenu → menus iPlug2, gfx_setcursor, glisser-déposer │
├────────────────────────────────────────────────────────────────────────────────┤
│ jsfx::Engine (framework/engine)   sans dépendance iPlug2, testé par ctest     │
│ jsfx::core = ysfx + EEL2 + LICE + fft, compilés sur le WDL d'iPlug2           │
└────────────────────────────────────────────────────────────────────────────────┘
générés à la compilation (cmake/) :
  jsfx_meta → jsfx_meta.h (E/S, taille UI) + jsfx_params.inc (table des paramètres)
  embed_files.cmake → jsfx_embedded.cpp (tout le dossier du plugin)
  config.h + Info.plist ← plugin.json
```

**Fichiers embarqués.** Le dossier du plugin est compilé dans le binaire, puis
extrait une seule fois au premier chargement dans un cache utilisateur nommé
d'après le hash du contenu (`%LOCALAPPDATA%` ou `~/Library/Caches`, puis
`<fabricant>/<plugin>/<hash>/`). ysfx le charge ensuite comme n'importe quel
JSFX : `import`, `file_open()`, `gfx_loadimg()` fonctionnent, et une nouvelle
version du plugin n'utilise jamais d'anciens fichiers.

**Threads.** Le thread audio ne bloque jamais (`try_lock` : un bloc est rendu
silencieux pendant un changement d'état). `@gfx` tourne sur son propre thread,
en parallèle de `@sample`, comme dans REAPER. Les opérations lourdes (état,
rechargement) prennent les verrous gfx puis audio, dans cet ordre, et
débloquent au besoin un `gfx_showmenu()` en attente pour éviter tout
interblocage.

## Un seul WDL

iPlug2 et ysfx embarquent chacun une copie **différente** du WDL de Cockos
(`ptrlist.h`, `wdlstring.h`, EEL2, LICE, SWELL...). Les compiler toutes les
deux dans un même binaire donnerait deux définitions des mêmes classes inline
(violation ODR silencieuse) et des symboles en double.

Ici, **seul le WDL d'iPlug2 est compilé**. La copie de ysfx
(`third_party/ysfx/thirdparty/WDL`) n'est jamais compilée ni mise dans un
chemin d'include, et chaque unité de compilation n'existe qu'une fois :

| Fichier WDL | Compilé par iPlug2 (VST3/AU) | Compilé par ce framework |
|---|---|---|
| `win32_utf8.c` (Windows) | oui | non pour les plugins (seulement outils/tests) |
| `fft.c`, `eel2/*`, `lice/*` | non | oui, une fois (`jsfx::core`) |
| `swell/*` (macOS) | non (format APP uniquement) | oui, une fois par binaire, avec un préfixe ObjC unique |
| en-têtes (`ptrlist.h`...) | même version | même version |

Les deux seules modifications que ysfx apporte à son WDL (un `atof`
indépendant de la locale pour EEL2 et `LineParser`, sans quoi `0.5` est mal lu
sous une locale française) sont réappliquées au moment du configure sur des
**copies** de deux fichiers dans `build/` (`cmake/JsfxWdlPatches.cmake`) :
le sous-module iPlug2 n'est jamais modifié, et le configure échoue clairement
si le code visé change en amont. L'objet assembleur d'EEL2 est celui fourni
précompilé par le même WDL (ses constantes doivent correspondre à `ns-eel.h`),
donc NASM n'est pas nécessaire.

`jsfx_assert_single_wdl_target()` parcourt au configure toutes les sources de
chaque plugin (y compris celles injectées par les cibles iPlug2) et échoue si
une unité WDL apparaît deux fois, ou si une mise à jour d'iPlug2 se met à
compiler une unité que le framework fournit déjà.

## EEL2 sur Apple Silicon : JIT ou interprété

EEL2 compile normalement le JSFX en code machine (JIT). D'après les échanges
sur le forum JUCE, Logic Pro natif Apple Silicon exécute les AU dans un
service sandboxé qui interdit la mémoire exécutable : un plugin JIT y
planterait. Le réglage `JSFX_EEL2_MODE` choisit donc le moteur **par
architecture**, à l'intérieur même du binaire universel :

| `JSFX_EEL2_MODE` | arm64 macOS | x86_64 macOS / Windows |
|---|---|---|
| `AUTO` (défaut) | interprété | JIT |
| `JIT` | JIT | JIT |
| `PORTABLE` | interprété | interprété |

Ordre de grandeur mesuré sur le synthé d'exemple (8 voix, x86_64) : 40× le
temps réel en JIT, 7× en interprété. Si vos plugins ne visent pas Logic, ou
si vous vérifiez que vos hôtes autorisent le JIT, `-DJSFX_EEL2_MODE=JIT`
redonne toute la vitesse.

## CI (`.github/workflows/build.yml`)

* push / pull request : build Windows (VST3 x64) et macOS (VST3 + AU
  universels), tests du moteur, `auval` sur chaque AU ;
* artefacts téléchargeables `plugins-windows` et `plugins-macos` ;
* tag `v1.2.3` : les plugins zippés sont publiés dans une release GitHub.

Signature et notarisation macOS ne sont pas incluses (il faut un compte
développeur Apple) ; les binaires de la CI sont signés ad hoc, suffisant pour
les tests.

## Limites connues

* Code vérifié sous Linux uniquement : le moteur, le générateur, l'embarquement
  et les tests y compilent et passent (JIT et interprété), et les sources
  iPlug2 du plugin passent une vérification syntaxique contre les en-têtes
  d'iPlug2. La première compilation Windows/macOS réelle se fera dans la CI :
  surveillez le premier run.
* Ajouter ou retirer la section `@gfx` pendant un rechargement à chaud
  nécessite de rouvrir la fenêtre du plugin.
* `slider_show()` (visibilité dynamique) n'est pas reflété dans l'interface
  générique ; les presets `.rpl` ne sont pas exposés comme presets d'usine.
* Le rendu de `@gfx` utilise NanoVG (backend par défaut d'iPlug2) ; Skia n'est
  pas géré.

## Licences

iPlug2 et WDL : licence zlib/WDL. ysfx : Apache-2.0 (les fichiers ysfx
compilés restent sous cette licence). Roboto : Apache-2.0. Le code de ce
framework est à vous. Vérifiez la licence de tout JSFX tiers que vous
embarquez (les JSFX fournis avec REAPER ont leurs propres licences).
