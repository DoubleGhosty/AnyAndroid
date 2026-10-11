# Code d'hôte d'AnyAndroid (Java)

Ce dossier contient le code Java qu'AnyAndroid exécute dans le processus de l'application, au-dessus
du vrai `framework.jar` d'AOSP : les services système locaux de l'[ADR-006](../../docs/adr/ADR-006-demarrage-applications.md).

- `java/` : le code d'hôte, compilé par `javac --release 17` puis D8 en `anyandroid-host.dex`,
  placé à côté de `anyandroid.exe`. La CLI le charge après le chemin de classes d'amorçage
  (`exec --bootclasspath`, option `--host-dex` pour un autre fichier).
- `stubs/` : des **déclarations minimales** des classes du framework utilisées, pour `javac`
  seulement. Elles ne sont jamais incluses dans le DEX : à l'exécution, les classes viennent du
  `framework.jar` de l'image.

Règles des déclarations :

1. ne déclarer que ce que le code d'hôte utilise, avec la signature exacte du `framework.jar` ;
2. **jamais d'initialiseur constant** (`= 0`, `= "x"`) : `javac` recopierait la valeur dans le
   code d'hôte ; utiliser `Stubs.integer()`, `Stubs.string()`… ;
3. une méthode abstraite du framework que le code d'hôte n'implémente pas (souvent parce qu'elle
   est absente de la déclaration) répond une valeur neutre (`null`, `0`, `false`) et est
   journalisée une fois : une surcharge dont la signature ne correspond plus au jar se voit donc
   dans le journal (`méthode non implémentée par l'hôte AnyAndroid`).

Construction : option active quand un JDK ≥ 17 et le jar de R8 sont disponibles
(`-DAA_R8_JAR=<r8.jar>`, téléchargé et vérifié par `tools/fetch-android-inputs.sh --r8`).
