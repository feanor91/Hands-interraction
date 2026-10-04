# Procédure de test — étape 0 (couche transparente + journalisation)

**Objectif** : prouver que la couche se charge dans MSFS 2024 via VDXR, et lever les inconnues :
nom exact de l'exécutable, `XR_EXT_hand_tracking` exposé ou non, profils et actions déclarés par MSFS,
profil actif avec/sans manettes, conflit avec l'émulation de manettes de Virtual Desktop.

**Durée** : 20 à 30 minutes. **Risque** : faible (la couche ne modifie rien sauf l'ajout de l'extension de suivi des mains,
et se désinstalle en une commande).

## A. Préparation (PC, sans casque)

1. Compiler : voir README (`cmake ...` puis `cmake --build build --config Release`).
   *Si la compilation échoue : copiez-moi les premières erreurs.*
2. Tests du cœur : `ctest --test-dir build -C Release --output-on-failure`
   → attendu : « 100% tests passed ». *Sinon : copiez la sortie.*
3. Installer (PowerShell 64 bits **administrateur**) :
   `powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1`
   → attendu : « Installation terminee. » et une ligne « Registre : ... = 0 (active) ».
4. Laissez `hands.ini` tel quel (`discovery_mode = true`) pour ce premier test.

## B. Test dans le casque

Notez avant de commencer (je les demande dans le rapport) :
* version du Virtual Desktop Streamer et de l'application Quest ;
* les réglages Virtual Desktop liés aux **mains / contrôleurs** (noms exacts et état : suivi des mains, « utiliser les mains comme contrôleurs », etc.) ;
* si un autre outil OpenXR est installé (OpenXR Toolkit, OpenXR Tools, etc.).

**Test 1 — avec manettes**
1. Fermez MSFS. Lancez Virtual Desktop, connectez-vous au PC (VDXR doit être le runtime OpenXR actif).
2. Lancez MSFS 2024 (version Microsoft Store), passez en mode VR, lancez un vol et attendez d'être dans le cockpit (≈ 1 minute).
3. Avec les **manettes**, pointez et actionnez 2 ou 3 éléments du cockpit (gâchette), pour que MSFS utilise ses actions.
4. **Pendant que MSFS tourne**, ouvrez PowerShell et lancez :
   `powershell -ExecutionPolicy Bypass -File .\scripts\check-process.ps1 *> $env:USERPROFILE\Desktop\check-process.txt`
5. Quittez MSFS normalement (menu), puis copiez `C:\ProgramData\HandsLayer\logs\` ailleurs (ou gardez-le).

**Test 2 — mains nues (suivi des mains actif)**
1. Posez les manettes (suivi des mains actif sur le Quest ; réglage Virtual Desktop selon ce que vous aviez noté).
2. Relancez MSFS, mode VR, cockpit. Bougez les mains devant vous 30 secondes (sans manette).
3. Quittez MSFS normalement. Copiez de nouveau `logs\` (renommez le dossier du test 1 avant, car le journal est renouvelé à chaque lancement : le précédent devient `*.prev.log`).

## C. Ce que vous devez observer

| À regarder | Attendu | Signification |
|---|---|---|
| Fichier `logs\layer_<nom>.log` créé | oui | La couche s'est chargée et a pu écrire. **Si absent : c'est le point bloquant principal.** |
| Dans le jeu | aucun changement visible, aucun plantage, pas de ralentissement | Transparence |
| Ligne `processus : ...` | chemin complet de l'exe de MSFS | **Nom exact de l'exécutable** |
| `XR_EXT_hand_tracking proposee par le runtime` | OUI / NON | VDXR expose-t-il le suivi des mains aux couches ? |
| `*** supportsHandTracking = ... ***` | OUI / NON | Le suivi des mains marche-t-il pour cette session ? |
| `creation du hand tracker gauche/droite -> XR_SUCCESS` | succès | Le suivi des mains est exploitable |
| Lignes `>>> PROFIL SUGGERE PAR LE JEU` | un ou plusieurs profils | **Profils et actions déclarés par MSFS** |
| Lignes `profil actif sur /user/hand/left|right` | profil Touch ou « AUCUN » | Contrôleur vu par MSFS ; **conflit avec VD** si un profil est actif alors que vous n'avez pas de manette |
| Lignes `xrCreateActionSpace ... decalage pos=` | valeurs | **Indice sur le point de contact** |
| Ligne `API graphique du jeu : Direct3D 12` | D3D12 | Confirme la cible de l'étape 3 |

## D. Ce que vous me rapportez

1. `layer_*.log` du test 1 et du test 2 (collés en entier ; s'ils sont trop longs, envoyez les fichiers).
2. `check-process.txt`.
3. Les notes de la partie B (versions, réglages Virtual Desktop).
4. Vos observations : plantage ? ralentissement ? comportement différent d'une manette ? le profil actif en mode « mains » ?
5. Les sorties des étapes A.1 à A.3 **si** elles ont posé problème.

## E. Si ça ne marche pas

| Symptôme | Premières pistes |
|---|---|
| Aucun log | `check-process.ps1` section 3 (valeur `= 0` ?) et 4 (chercher aussi `%TEMP%\HandsLayer\` et `%LOCALAPPDATA%\Packages\*\LocalCache\`). MSFS a peut-être son propre chargeur OpenXR ou un dossier non lisible. Envoyez-moi la sortie. |
| MSFS plante ou n'entre pas en VR | Lancez `uninstall.ps1`, vérifiez que MSFS refonctionne, puis envoyez-moi le log s'il existe. |
| Log présent mais sans ligne `PROFIL SUGGERE` | Normal si MSFS n'a pas encore démarré la VR ; sinon envoyez le log entier. |
| Désinstaller | `powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1 -RemoveFiles` |

## F. Ensuite

Selon vos logs : je verrouille la liste d'exécutables (`discovery_mode = false`), j'adapte l'étape 1 aux profils/actions réellement
déclarés par MSFS, et je règle le conflit éventuel avec Virtual Desktop.
