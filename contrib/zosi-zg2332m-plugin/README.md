# hisilicon.so V2 — plugin AE Majestic pour Hi3516EV100 / SC2235P

Extension du plugin d'origine (`../custom.c`) pour éviter le colFPN de nuit
et laisser la config AE modifiable sans recompiler.

## Fichiers

- `custom.c` — source du plugin V2
- `hisilicon.so` — binaire compilé (armv5-eabi musl, 18 KB)
- `majestic-ae.conf` — fichier de config à déposer dans `/etc/majestic-ae.conf`
- `deploy.sh` — script de déploiement RAM-only (bind-mount, sans écrire l'overlay)

## Ce qui change par rapport à V1

| V1 | V2 |
|---|---|
| profils AE codés en dur en C | lus depuis `[AE_Plugin]` / `[AE_Plugin_<nom>]` dans le .conf |
| `stockae day\|night` (deux profils bakés) | + `profile <nom>` (n'importe quelle section) |
| pas de commande pose | `expmax <µs>` (symétrique de `gainmax`) |
| `route` : lecture seule | `route`, `route reload`, `route clear`, `route <t:g,...>` |
| — | `fpn on\|off\|calibrate` (voir limitation) |
| après cold-boot il fallait envoyer `gainmax 15360` | cold-boot applique déjà `[AE_Plugin]` |

## Config `/etc/majestic-ae.conf`

Voir le fichier. Section `[AE_Plugin]` = profil appliqué au boot.
Clé `Boot` :
- `current` (défaut) — applique cette section elle-même
- `none` — ne touche à rien
- `<nom>` — applique `[AE_Plugin_<nom>]`

Autres sections fournies : `AE_Plugin_night_stock`, `AE_Plugin_day_stock`, `AE_Plugin_night_safe`.

## Limitations

- **FPN indisponible** sur ce firmware.
  - `HI_MPI_ISP_GetFPNAttr` → `0xa01c8042` (module non initialisé)
  - `HI_MPI_ISP_FPNCalibrate` avec `stFpnCaliFrame` vide → `0xa01c8003`
    (paramètre invalide — le champ attend un `VIDEO_FRAME_INFO_S` avec un
    buffer MMZ pré-alloué, pas un simple struct zéro)
  - Pour l'activer proprement depuis le plugin il faudrait ~100 lignes
    supplémentaires : `HI_MPI_SYS_MmzAlloc_Cached` d'un buffer ~4 KB (line-mode)
    ou ~2 MB (frame-mode), remplissage des `PhyAddr/VirAddr/Stride/PixelFormat`,
    cycle capuchon physique, puis `SetFPNAttr(bEnable=1)`.
  - Comme le `gainmax 15360` traite déjà la cause visible (raies) sans avoir
    besoin de FPN, on garde ce chantier en tiroir et on ne l'ouvre que si
    un cas concret montre que le cap ne suffit pas.
- **Route AE en écriture** : l'API répond `0xa01c8003` sur les valeurs
  arbitraires. Il faut connaître les IntTime discrètes du SC2235P pour
  produire un route valide. `route reload` depuis `[AE_Route] Nodes=...`
  fonctionne dès que les valeurs sont bonnes.
- **Persistance** : le déploiement actuel utilise bind-mount depuis /tmp
  (RAM). Un reboot annule tout. Pour la persistance il faudra soit libérer
  de l'overlay jffs2, soit ajouter un mount permanent.

## Build

Toolchain : `arm-buildroot-linux-musleabi-` (musl, armv5). Bootlin fournit
un binaire prêt à l'emploi :
https://toolchains.bootlin.com/downloads/releases/toolchains/armv5-eabi/tarballs/armv5-eabi--musl--stable-2024.05-1.tar.xz

Dépendances (git clone) :
- `git@github.com:OpenIPC/majestic-plugins.git` (pour `plugin.c` et `plugin.h`)
- `git@github.com:OpenIPC/openhisilicon.git` (pour les headers MPP cv300)

Compilation :
```sh
arm-linux-gcc custom.c ../majestic-plugins/plugin.c \
  -I../majestic-plugins \
  -I../openhisilicon/kernel/include/hi3516cv300 \
  -o hisilicon.so -Os -s -shared -fPIC
```
