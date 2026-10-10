# Sécurité

AnyAndroid est un **prototype de recherche**. Il n'offre pas de bac à sable fort : n'exécutez pas
d'APK auxquels vous ne faites pas confiance. Le modèle de menace et l'état des mesures sont décrits
dans [docs/security/threat-model.md](docs/security/threat-model.md).

## Signaler une vulnérabilité

Merci de ne pas ouvrir d'issue publique pour une faille exploitable (plantage d'un parseur sur un
fichier forgé, lecture hors limites, contournement d'une limite de ressources…). Contactez le
mainteneur en privé, en joignant si possible un fichier de reproduction minimal. Tout correctif
ajoute un cas de test permanent (fixture ou graine de fuzzing).

## Hors périmètre

- Le contournement des contrôles d'intégrité de tiers (Play Integrity, SafetyNet, DroidGuard,
  détection d'émulateur) n'est **pas** un objectif du projet (voir l'ADR-002).
