```markdown
# part 1 — Simulation Wi-Fi minimale (1 AP + 1 STA) avec Ping + PCAP (ns-3)

Ce dépôt implémente le **part 1** : une simulation Wi-Fi minimale sous **ns-3** avec :
- **1 point d’accès (AP)** et **1 station (STA)**,
- configuration Wi-Fi avec un **SSID** paramétrable,
- **adresses IPv4** sur un même sous-réseau,
- **Ping** de la STA vers l’AP (preuve de connectivité),
- génération de **captures PCAP**,
- sorties rangées dans `results/` (répétable et propre).

---

## 1) Structure du dépôt

```

wifi-ter-sim/
README.md
docs/
part1.pdf
scenarios/
p1_minimal_wifi.cc
results/
p1/
raw/     # PCAP
logs/    # résumés / logs
plots/   # réservé (non utilisé au part 1)

````

**Règle** :
- le code est dans `scenarios/`
- les sorties sont sous `results/`

---

## 2) Pré-requis

- Linux (bash/zsh)
- ns-3 installé/cloné localement (ex. `~/ns-3`) et compilable
- Compilateur C++ + CMake/Ninja (selon la distribution ns-3)

Vérifier ns-3 :
```bash
cd ~/ns-3
./ns3 show version
./ns3 run hello-simulator
````

Vous devez voir `Hello Simulator`.

---

## 3) Scénario du part 1 (p1_minimal_wifi.cc)

Le scénario :

* crée **2 nœuds** : 1 AP + 1 STA,
* place l’AP en **(0,0,0)** et la STA en **(distance,0,0)** (positions fixes),
* configure le Wi-Fi (SSID identique côté AP/STA),
* installe la pile IP et attribue des **IPv4**,
* lance un **Ping STA → AP** à partir de **t = 1 s**,
* produit des **PCAP** dans `outDir/raw/`,
* écrit un résumé dans `outDir/logs/summary.txt`.

---

## 4) Paramètres (CommandLine)

Le programme accepte :

* `--ssid` : nom du réseau Wi-Fi (SSID)
* `--simTime` : durée de simulation (secondes)
* `--distance` : distance AP–STA (mètres)
* `--pcap` : activer/désactiver les captures PCAP (`true/false`)
* `--outDir` : répertoire de sortie (créera `raw/`, `logs/`, `plots/`)

Aide :

```bash
cd ~/ns-3
./ns3 run "scratch/p1_minimal_wifi --PrintHelp"
```

---

## 5) Compilation + exécution (procédure recommandée)

> ns-3 exécute le scénario depuis `scratch/`.
> On copie donc le fichier de ce dépôt vers `~/ns-3/scratch/`.

### 5.1 Copier le scénario dans ns-3

Depuis la racine du dépôt :

```bash
cp scenarios/p1_minimal_wifi.cc ~/ns-3/scratch/p1_minimal_wifi.cc
```

### 5.2 Compiler

```bash
cd ~/ns-3
./ns3 build
```

### 5.3 Exécuter (commande principale)

On recommande un `outDir` **absolu** pour écrire directement dans ce dépôt :

```bash
./ns3 run "scratch/p1_minimal_wifi \
  --outDir=$(realpath ~/wifi-ter-sim/results/p1) \
  --pcap=true \
  --ssid=wifi-demo \
  --distance=5 \
  --simTime=10"
```

Résultat attendu :

* sorties Ping dans le terminal (0% de perte),
* fichiers `.pcap` créés dans `results/p1/raw/`,
* `summary.txt` dans `results/p1/logs/`.

---

## 6) Vérification des sorties

### 6.1 PCAP

```bash
ls -lh results/p1/raw
```

Exemples :

* `wifi_<ssid>_d<distance>m_ap-0-0.pcap`
* `wifi_<ssid>_d<distance>m_sta-1-0.pcap`

### 6.2 Résumé

```bash
cat results/p1/logs/summary.txt
```

---

## 7) Analyse PCAP (optionnel)

Avec Wireshark :

```bash
wireshark results/p1/raw/wifi_wifi-demo_d5m_ap-0-0.pcap
```

Filtre ICMP :

* `icmp`

---

## 8) Dépannage

### 8.1 “Invalid command-line argument: --”

Ne pas utiliser le séparateur `--` avec `./ns3 run` dans cette configuration.
Utiliser exactement la commande de la section 5.3.

### 8.2 Crash “Invalid Address” (ping.cc)

Le Ping doit recevoir une adresse IPv4/IPv6 valide (pas un InetSocketAddress).
Le scénario utilise `PingHelper ping(apIp);`, ce qui est correct.

### 8.3 Aucun PCAP généré

* Vérifier `--pcap=true`
* Vérifier les droits d’écriture dans `--outDir`
* Vérifier `results/p1/raw/`

### 8.4 Problème avec 802.11ax

Si `WIFI_STANDARD_80211ax` n’est pas disponible dans votre build ns-3, remplacer
dans le code par `WIFI_STANDARD_80211ac` ou `WIFI_STANDARD_80211n`, puis recompiler.

---

## 9) Nettoyage

Supprimer uniquement les sorties du part 1 :

```bash
rm -rf results/p1
mkdir -p results/p1/{raw,logs,plots}
```

---

## 10) Livrables (part 1)

* `scenarios/p1_minimal_wifi.cc`
* `results/p1/raw/*.pcap` (au moins un fichier)
* `results/p1/logs/summary.txt`
* `docs/part1.pdf` (rapport 1–2 pages : topo, paramètres, preuve ping, preuve pcap)
* `README.md` (ce fichier)

