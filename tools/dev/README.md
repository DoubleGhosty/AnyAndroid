# Outils de développement

Aides pour travailler sur le vrai framework d'Android (G2/G3). Elles lisent des fichiers
extraits de l'image système, qui ne vont jamais dans le dépôt (`tools/fetch-android-inputs.sh`
les place hors du dépôt, dans le dossier noté ici `$AA_INPUTS`).

| Outil | Usage |
|---|---|
| `jarapi.py` | API réelle d'un jar ou d'un DEX : classes, méthodes (drapeaux, descripteurs), natives, champs avec leurs constantes, vidage complet. Sert à vérifier une signature native ou une déclaration de `runtime/android-host/stubs/` avant de l'écrire. |
| `dis.sh` | Désassemble une classe (`anyandroid inspect --disasm`), avec un cache par classe ; filtre par nom de méthode. |
| `launch.sh` | Recompile puis lance le lanceur J4 (`aa.host.Launcher`) avec `--trace-services` ; affiche la fin de la sortie et la chaîne d'exceptions. |

Extraire les jars une fois :

```
wine build/apps/cli/anyandroid.exe image cat Z:$AA_INPUTS/aosp17/system.img \
    /system/framework/framework.jar --output Z:$AA_INPUTS/dex/framework.jar
```

Exemples :

```
python3 -I tools/dev/jarapi.py $AA_INPUTS/dex/framework.jar natives 'android.graphics.Matrix$ExtraNatives'
python3 -I tools/dev/jarapi.py $AA_INPUTS/dex/framework.jar fields 'android.view.Display$Mode'
tools/dev/dis.sh $AA_INPUTS/dex/framework.jar android.graphics.Typeface preloadFontFile
AA_INPUTS=/chemin/entrees tools/dev/launch.sh
```

Les entrées d'une image Android sont des données non fiables : lancer Python avec `-I`.
