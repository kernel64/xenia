# Harissa64 : faire tourner le dynarec dans Xenia

Étude et prototype du 2026-10-12, branche `harissa-dynarec` de ce clone (commit `718bbb715`, basé sur `master` `95a5c3ee2`). Rien n'a été poussé, `master` n'a pas été touché.

## En bref

- **Ça marche.** Avec le prototype (≈420 lignes dans `src/xenia/cpu`, `kernel` et `emulator.cc`), le recompilateur de Harissa64 tourne dans Xenia : OoT, GoldenEye et Conker démarrent et jouent leur intro avec `cpu=dynarec`.
- **Le lockstep passe.** `mode=jittest` sur OoT : 10 s émulées sans aucune divergence entre le recompilateur et l'interpréteur (4,2 millions de blocs exécutés).
- **Les images sont justes.** OoT, VI 150 / 250 / 400 : captures identiques octet pour octet entre le dynarec et l'interpréteur de Harissa, dans Xenia. GoldenEye, VI 300 : identique ; VI 600 : un reflet du logo diffère très légèrement (FPU rapide `fastfpu=1` contre interpréteur, écart attendu, voir le CLAUDE.md de Harissa).
- **C'est rapide.** OoT tourne à 50 VI/s (pleine vitesse PAL, limitée par le pacing), contre 8 VI/s avec l'interpréteur. Le thread CPU prend ~14 ms par image au lieu de ~125 ms. Conker : 60 VI/s sur les logos, 18 à 45 VI/s dans les cinématiques.
- **Limite actuelle : le cache de code de Xenia ne se vide jamais.** Chaque réécriture de code par Harissa (bloc compilé, saut lié ou délié) fait retraduire des fonctions, et l'ancien code x64 reste. Il y a 256 Mo en tout : OoT en utilise 48 Mo après 3000 VI, Conker ~144 Mo après 5000 VI (~83 s de jeu). Conker tiendrait ~2 min 30. C'est assez pour des tests courts (régressions, lockstep), pas pour une longue partie. La suite logique est de recycler cette mémoire (voir « Prochaines étapes »).

## 1. Compiler Xenia sur cette machine

Aucune installation admin n'a été nécessaire. Trois problèmes et leurs correctifs (dans `xenia-build`, commités sur la branche) :

1. `tools/vswhere/vswhere.exe` avec des `/` : `CreateProcess` ne le trouve pas sur ce Windows (`FileNotFoundError`). Correctif : `os.path.join`.
2. VS 2026 renvoie `productLineVersion = 18` au lieu d'une année. Le script prenait ça pour « pas de VS ». Correctif : 18 → 2026.
3. premake-core ne connaît pas `vs2026`. Correctif : on génère avec l'action `vs2022`.

Ensuite, `xb build` choisit l'instance « 18\Insiders », qui est incomplète (pas de `VC\v180\Microsoft.Cpp.Default.props`). Le toolset v143 que demandent les projets n'est pas installé non plus (seul v145 l'est). On appelle donc MSBuild nous-mêmes.

Commandes (PowerShell, depuis `E:\vs-projects\xenia`) :

```powershell
# 1. Sous-modules (clone superficiel, quelques minutes)
git -c fetch.recurseSubmodules=on-demand submodule update --init --depth 1 --jobs 8
# 2. Projets VS (premake se compile tout seul au premier lancement)
python xenia-build premake
# 3. Compilation Release (~10 min la première fois, 1 à 3 min ensuite)
cmd /c '"E:\tools\Microsoft Visual Studio\2026\VC\Auxiliary\Build\vcvars64.bat" >nul && msbuild build\xenia.sln /nologo /m /v:m /p:Configuration=Release /p:Platform=Windows /p:PlatformToolset=v145 /t:src\xenia-app'
```

Le résultat est `build\bin\Windows\Release\xenia.exe` (+ `xenia.pdb`). La cible de la solution s'appelle `src\xenia-app` (dossier de solution). J'ai choisi Release : c'est la configuration la plus rapide à l'exécution, et elle garde les PDB.

Pour lancer Harissa64 (dossier de test : `E:\h64v2_xenia\dynarec\game` pour le xex, `test.z64`, `ge.zip`, `conker.zip` et l'ini ; `E:\h64v2_xenia\dynarec\work` comme dossier de travail) :

```powershell
Start-Process E:\vs-projects\xenia\build\bin\Windows\Release\xenia.exe -WorkingDirectory E:\h64v2_xenia\dynarec\work -ArgumentList '--log_file=E:\h64v2_xenia\dynarec\work\xenia.log','--mount_cache=true','--d3d12_readback_resolve=true','"E:\h64v2_xenia\dynarec\game\harissa64v2.xex"'
```

- `--mount_cache=true` : sans lui, `cache:\` n'existe pas dans ce Xenia, et Harissa ne peut écrire ni son log ni ses captures.
- `--d3d12_readback_resolve=true` : c'est le nom de l'option dans Xenia « master ». Dans Canary, elle s'appelle `readback_resolve`, et l'ancien nom fait échouer le lancement sans rien dire.
- Ini utilisé : `xenia=1`, `mode=play`, `cpu=dynarec`, `hle=1`, `shots=...`, `exitafter=...` (et `rom=game:\ge.zip` pour un autre jeu).
- Le script de test que j'ai utilisé (ini, lancement, copie du log et des BMP) est `runx.ps1`, dans le scratchpad de la session. Il fait ~20 lignes et se refait facilement.
- Ce Xenia écrit sa config dans `C:\Users\aymen\Documents\Xenia\xenia.config.toml`. Ce dossier a été créé par mes essais. La config enregistre les cvars, donc une valeur restée là l'emporte sur la valeur par défaut d'un nouveau build. J'y ai remis `dynamic_code_window = 0`.
- Code de sortie `0xC0000374` (tas corrompu) quand le titre quitte : il arrive aussi avec l'interpréteur. C'est un problème de Xenia à la fermeture, sans lien avec le prototype.

## 2. Pourquoi le dynarec ne marchait pas dans Xenia

Le fonctionnement de Xenia : `Processor::ResolveFunction` (`src/xenia/cpu/processor.cc`) cherche le module qui contient l'adresse (`XexModule::ContainsAddress` : les pages CODE du XEX), le scanner PPC (`ppc/ppc_scanner.cc`) devine où finit la fonction, puis `PPCTranslator` → HIR → backend x64. Le code est placé dans un cache qu'on ne fait que remplir (`backend/x64/x64_code_cache.cc`, 256 Mo à 0xA0000000). Une table d'indirection (`0x80000000 + adresse invitée`) donne le code x64 de chaque adresse, ou un thunk qui appelle `ResolveFunction`. Rien n'invalide jamais une fonction déjà traduite.

Ce qui bloquait Harissa64 (`.jitc` = 32 Mo à 0x823C0000, dans la plage CODE 0x820D0000–0x843C0000 du XEX) :

1. **Pages en lecture seule.** `XexModule::LoadContinue` protège les pages CODE en lecture seule (`heap->Protect`), alors que sur la console elles sont inscriptibles. La première écriture de code généré (`stwu` à 0x823C0000) plante. C'est le crash dump du Xenia non modifié (PC 0x820D638C).
2. **`KeSweepIcacheRange` n'existe pas** (juste déclaré dans `xboxkrnl_table.inc`), donc l'appel tombe sur un import non implémenté. `MmQueryAddressProtect` et `MmSetAddressProtect` sont implémentés dans ce Xenia « master » (`xboxkrnl_memory.cc`) ; c'est sans doute Canary (mars 2025) qui plantait dessus.
3. **Aucune invalidation.** Une fonction traduite reste dans `EntryTable`, dans la table des symboles du module et dans la table d'indirection. En plus, `X64Emitter::Call` écrit en dur l'adresse x64 de la fonction appelée quand elle est déjà traduite.
4. **Modèle appel/retour.** Xenia traduit `bl` en `call` x64 et `blr` en `ret` quand la cible est l'adresse de retour du cadre, sinon en saut. `rtFcStore` et `rtFcLoad` de Harissa reviennent *après une table de données* (LR + N). Le cadre x64 de l'appelant restait alors sur la pile pour toujours : la pile grossit et l'exécution finit par repartir au mauvais endroit.
5. **Le scanner suit les branches lointaines.** Les chemins lents sont dans la zone froide, loin du code chaud : une « fonction » couvrait tous les blocs suivants. Les fonctions étaient énormes, chaque invalidation en touchait beaucoup, et le cache se remplissait vite.
6. **Capacité.** La table d'unwind est limitée à 100 000 fonctions, et un dépassement écrit hors du tableau (violation d'accès silencieuse dans `Win32X64CodeCache::PlaceCode`). Rien ne vérifie non plus que les 256 Mo du cache ne sont pas dépassés.

Note : l'ancien constat « appeler le buffer plante (null host call) » venait de tampons `VirtualAlloc`, donc hors module. `LookupFunction` ne trouve alors aucun module et renvoie null. Le prototype ne traite que les sections du XEX, ce qui est le cas de Harissa64.

## 3. Le changement (prototype, branche `harissa-dynarec`)

| Fichier | Changement |
|---|---|
| `cpu/xex_module.cc` (`LoadContinue`) | Les sections PE `MEM_EXECUTE` + `MEM_WRITE` (l'option `/SECTION:.jitc,ERW`) restent inscriptibles et sont déclarées « code dynamique » (`Processor::AddDynamicCodeRange`). Log : `XEX section .jitc (823C0000, 33554432 bytes) is writable + executable`. |
| `kernel/xboxkrnl/xboxkrnl_memory.cc` | `KeSweepIcacheRange(addr, size)` → `Processor::InvalidateCodeRange`. `KeSweepDcacheRange` ne fait rien. |
| `cpu/processor.{h,cc}` | Plages dynamiques, index des fonctions traduites par page de 4 Ko (début, fin). `InvalidateCodeRange` supprime chaque fonction qui chevauche la plage : entrée de `EntryTable`, symbole du module, case d'indirection remise sur le thunk de résolution. L'objet `Function` et son code x64 restent en vie (ils peuvent être en cours d'exécution). Statistiques dans le log. |
| `cpu/entry_table.*`, `cpu/module.*` | `EntryTable::Remove`, `Module::ForgetFunction`. |
| `cpu/backend/backend.h`, `x64_backend.*`, `x64_code_cache.h` | `ResetIndirection(addr)`. |
| `cpu/backend/x64/x64_emitter.cc` (`Call`) | Une cible en code dynamique passe toujours par la table d'indirection (jamais d'adresse x64 écrite en dur). |
| `cpu/ppc/ppc_context.h`, `ppc_emit_control.cc`, `x64_emitter.cc` (`CallIndirect`) | **Retour ailleurs qu'au site d'appel.** Un `blr` dont la cible n'est pas l'adresse de retour du cadre, dans une fonction appelée depuis du code dynamique : on écrit la cible dans `PPCContext::dynamic_resume` et on fait un vrai `ret`. Après chaque appel `bl`/`bctrl` en code dynamique, l'appelant lit `dynamic_resume` ; s'il n'est pas nul, il y saute (saut terminal qui vérifie aussi « est-ce un retour ? »). La pile x64 reste équilibrée, et un retour qui doit remonter plusieurs niveaux (sortie vers le dispatcher C) remonte cadre par cadre. |
| `cpu/ppc/ppc_scanner.cc`, `cpu_flags.*` | En code dynamique, seules les cibles à moins de `dynamic_code_window` octets du début prolongent la fonction (défaut 0), et un `b` lointain la termine. Les fonctions font ~70 octets invités (un morceau de bloc), et un saut lié réécrit ne retraduit que ce petit morceau. |
| `cpu/backend/x64/x64_code_cache*` | 1 000 000 de fonctions au plus au lieu de 100 000 ; `FatalError` au lieu d'un débordement ; log `Code cache: N MB of host code, M functions` tous les 16 Mo. |
| `emulator.cc` | Diagnostic : une violation d'accès hors du code invité est écrite dans `xenia_host_crash.txt` (pile x64 brute ; à symboliser hors ligne avec dbghelp + `xenia.pdb`). C'est ce qui a montré le dépassement de la table d'unwind. |

Pour un jeu normal, rien ne change : tout ce qui est nouveau ne s'active que si le XEX a une section W+X. La vérification ajoutée dans `CallIndirect` n'est émise que s'il existe des plages dynamiques, et la lecture de `dynamic_resume` seulement dans du code dynamique.

Côté Harissa64, aucun changement n'est nécessaire : `xenia=1` + `cpu=dynarec` suffisent. `BenchCodeMemory` est déjà sauté avec `xenia=1`.

## 4. Mesures (i7-11800H, D3D12, Release)

| Test | Dynarec (prototype) | Interpréteur Harissa | Remarques |
|---|---|---|---|
| OoT PAL, titre + démo, 3000 VI | 50 VI/s, CPU ~14 ms/image | 8 VI/s, CPU ~125 ms/image | VI 150/250/400 identiques à l'interpréteur ; VI 3000 identique d'une exécution à l'autre. Cache : 48 Mo, ~46 000 fonctions. |
| GoldenEye, 1200 VI | 57–60 VI/s | — | VI 300 identique ; VI 600 : léger écart sur le reflet du logo (FPU rapide). |
| Conker, 5000 VI | 60 VI/s (logos), 18–45 VI/s (cinématiques, CPU 20–55 ms/image) | — | Démarrage IPL3 + RSP LLE, logos, pub « GAME1 ». 169 000 appels à `KeSweepIcacheRange`, 153 000 fonctions supprimées, 217 000 traduites, cache à 144 Mo. |

Réglage du scanner (Conker, 1500 VI) : avec une fenêtre de 8 Ko, 353 octets invités par fonction et 224 Mo de cache ; avec la fenêtre à 0 + fin sur `b` lointain, 72 octets et 80 Mo, à la même vitesse.

**Test lockstep** (`mode=jittest`, OoT, 10 s émulées, le recompilateur comparé à l'interpréteur après chaque bloc) : `lockstep OK: 4243680 dispatches, 18287 blocks compiled`, sans aucune divergence, en 113 s. Sur les mêmes 10 s émulées (sans dessin RDP) : 77 s pour l'interpréteur, 21,7 s pour le recompilateur. Ce mode est le plus utile pour tester le dynarec dans Xenia. Il faut l'arrêter à la main une fois la ligne `10 emulated seconds` écrite : le titre reste ouvert sur son écran de résultat.

## 5. Risques et limites

- **Cache de code non recyclé** (le point principal). Xenia n'a ni éviction ni libération : chaque retraduction ajoute du code. Les jeux qui lient et délient beaucoup (Conker, DK64), et chaque vidage complet du cache de Harissa (toutes les ~40 s sur DK64/Conker), consomment vite les 256 Mo. Arrivé au bout : `FatalError` propre (plus de corruption).
- **Code périmé encore en cours d'exécution.** Une fonction invalidée pendant qu'elle tourne (par un helper appelé depuis elle) finit son exécution avec l'ancienne traduction. Pour Harissa, c'est sans conséquence : un bloc ne réécrit jamais ses propres instructions restantes, et une invalidation fait sortir le bloc.
- **Un seul thread exécute du code dynamique** (le thread CPU de Harissa). L'invalidation est protégée par un verrou, mais une fonction en cours de traduction par un autre thread n'est pas supprimée. Les objets `Entry` supprimés sont laissés en mémoire volontairement (petite fuite).
- **Seulement les sections du XEX.** Le code écrit dans de la mémoire `VirtualAlloc`/`XPhysicalAlloc` n'est pas géré (il faudrait un « module dynamique » qui couvre ces plages).
- **`icbi` reste un nop.** Un titre qui invaliderait avec `icbi` seul, sans `KeSweepIcacheRange`, ne serait pas vu.
- **Vitesse.** Plus lent que la console sur les contenus lourds (cinématique de Conker : 40–55 ms CPU contre ~12,5 ms sur la console) : retraductions, appels de helpers via les thunks. Pour mesurer la vitesse, la console reste la référence.
- **Taille de `PPCContext`** : +64 octets, pour garder l'alignement sur 64.

## 6. Prochaines étapes

1. **Recycler la mémoire du code dynamique** (pour les longues sessions) :
   - une zone séparée dans le cache x64 pour les fonctions dynamiques, avec sa propre table d'unwind Windows (`RtlAddGrowableFunctionTable` / `RtlDeleteGrowableFunctionTable`) et sa propre carte hôte → fonction ;
   - quand elle est pleine, tout jeter (entrées, symboles, cases d'indirection des fonctions dynamiques) et repartir de zéro, à un moment sûr : dans `KeSweepIcacheRange`, si une remontée de la pile x64 (`RtlCaptureStackBackTrace`) ne montre aucun cadre de cette zone. Sur Harissa, c'est le cas à chaque compilation de bloc et à chaque liaison faite par le dispatcher.
2. **Moins de retraductions sur les liaisons** : traduire le `b` de sortie d'un bloc par un saut via la case d'indirection de sa cible. Réécrire ce `b` ne demanderait alors que de retraduire une fonction d'une instruction, ce qui est presque déjà le cas avec la fenêtre à 0.
3. **Code hors module** (pour d'autres titres) : un module « dynamique » ajouté par `MmSetAddressProtect`/`NtProtectVirtualMemory`/`NtAllocateVirtualMemory` quand la protection demande l'exécution, et des fonctions créées à la demande dans ces plages.
4. **Porter le patch sur Xenia Canary** (la version de l'utilisateur, mars 2025). Le code CPU est proche mais a divergé (noms des options, cache de code), donc le patch ne s'appliquera sans doute pas tel quel. En attendant, le `xenia.exe` de cette branche suffit pour les tests de Harissa64.
5. Côté Harissa64 : documenter dans `docs-local/DEVELOPMENT.md` la procédure ci-dessus (exe, options `--mount_cache`, `--d3d12_readback_resolve`, `cpu=dynarec`), et ajouter un test scripté (OoT 400 VI, captures comparées à l'interpréteur).
