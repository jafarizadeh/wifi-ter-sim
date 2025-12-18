
# README — Part 2 (ns-3) : Baseline Wi-Fi + CSMA, UDP vs TCP, Goodput & RTT

## 1) Objectif
Ce Part implémente un scénario ns-3 avec :
- un lien **Wi-Fi** entre **STA ↔ AP**,
- un lien **CSMA (Ethernet)** entre **AP ↔ Server**,

puis compare **UDP** et **TCP** sur la même topologie.

**KPI / sorties produites** :
- **Goodput global** (en fin de simulation),
- **Goodput en série temporelle** (CSV),
- **RTT en série temporelle** (CSV) sous trafic,
- **FlowMonitor** (XML),
- **PCAP** (Wireshark).

---

## 2) Prérequis
### 2.1 Logiciels
- Linux (testé sous Ubuntu)
- ns-3 (testé avec **ns-3.46.x**, ex: `ns-3.46.1-128-...`)
- Python 3
- Python packages : `numpy`, `matplotlib`
  ```bash
  python3 -m pip install --user numpy matplotlib
````

### 2.2 Hypothèses de chemins

Ce README suppose :

* ns-3 : `~/ns-3`
* dépôt Part : `~/wifi-ter-sim`

Si vos chemins diffèrent, adaptez `NS3_DIR` et `REPO_DIR` dans les scripts.

---

## 3) Arborescence du dépôt

```
wifi-ter-sim/
  scenarios/
    p2_baseline.cc
  scripts/
    run_p2.sh
    plot_p2.py
  results/
    p2/
      udp/run1/{raw,logs,plots}
      tcp/run2/{raw,logs,plots}
```

**Note** : les sorties sont séparées par transport/run afin d’éviter tout écrasement de CSV/PCAP.

---

## 4) Topologie & plan d’adressage (rappel)

### 4.1 Noeuds

* `staNode` (client Wi-Fi)
* `apNode` (point d’accès)
* `serverNode` (serveur côté CSMA)

### 4.2 Liens

* Wi-Fi : STA ↔ AP
* CSMA : AP ↔ Server

### 4.3 Sous-réseaux IPv4 (obligatoires)

* Wi-Fi (AP–STA) : `10.1.0.0/24`
* CSMA (AP–Server) : `10.2.0.0/24`

Après l’assignation des IP, le routage est activé via :
`Ipv4GlobalRoutingHelper::PopulateRoutingTables()`.

---

## 5) Compilation (build)

### 5.1 Copier le scénario dans ns-3

ns-3 compile les programmes placés dans `scratch/` :

```bash
cp ~/wifi-ter-sim/scenarios/p2_baseline.cc ~/ns-3/scratch/p2_baseline.cc
```

### 5.2 Compiler

```bash
cd ~/ns-3
./ns3 build
```

---

## 6) Exécution (run)

### 6.1 Exécution automatisée (recommandée)

Script batch (UDP + TCP) :

* `~/wifi-ter-sim/scripts/run_p2.sh`

Exécution :

```bash
chmod +x ~/wifi-ter-sim/scripts/run_p2.sh
~/wifi-ter-sim/scripts/run_p2.sh
```

Sorties créées :

* UDP : `~/wifi-ter-sim/results/p2/udp/run1/`
* TCP : `~/wifi-ter-sim/results/p2/tcp/run2/`

### 6.2 Exécution manuelle (exemples)

#### UDP

```bash
cd ~/ns-3
./ns3 run "scratch/p2_baseline \
  --transport=udp \
  --ssid=wifi-demo \
  --distance=5 \
  --simTime=20 \
  --appStart=2 \
  --pktSize=1200 \
  --udpRate=50Mbps \
  --pcap=true \
  --flowmon=true \
  --seed=1 \
  --run=1 \
  --outDir=$(realpath ~/wifi-ter-sim/results/p2/udp/run1)"
```

#### TCP

```bash
cd ~/ns-3
./ns3 run "scratch/p2_baseline \
  --transport=tcp \
  --ssid=wifi-demo \
  --distance=5 \
  --simTime=20 \
  --appStart=2 \
  --pktSize=1200 \
  --tcpMaxBytes=0 \
  --pcap=true \
  --flowmon=true \
  --seed=1 \
  --run=2 \
  --outDir=$(realpath ~/wifi-ter-sim/results/p2/tcp/run2)"
```

### 6.3 Afficher l’aide (options disponibles)

```bash
cd ~/ns-3
./ns3 run "scratch/p2_baseline --PrintHelp"
```

---

## 7) Paramètres CommandLine

### 7.1 Paramètres généraux

* `--simTime` : durée totale de la simulation (s)
* `--appStart` : début du trafic principal (s)
* `--distance` : distance AP-STA (m)
* `--ssid` : SSID Wi-Fi
* `--outDir` : répertoire de sortie
* `--pcap` : activer/désactiver PCAP
* `--flowmon` : activer/désactiver FlowMonitor
* `--seed` : seed RNG
* `--run` : run RNG

### 7.2 Sélection du trafic

* `--transport=udp|tcp`

### 7.3 Paramètres trafic

* `--pktSize` : taille paquet (octets)
* UDP :

  * `--udpRate` : débit offert (ex: `50Mbps`)
* TCP :

  * `--tcpMaxBytes` : `0` = illimité

### 7.4 Mesures

* `--thrInterval` : période d’échantillonnage goodput (s), défaut `0.5`
* `--rttHz` : fréquence de sonde RTT (Hz), défaut `5`
* `--rttVerbose` : afficher le RTT sur la console (bool)

---

## 8) Sorties générées

Toutes les sorties sont écrites sous `--outDir` :

### 8.1 `raw/`

* `throughput_timeseries.csv`
  Colonnes : `time_s,throughput_bps`
* `rtt_timeseries.csv`
  Colonnes : `time_s,seq,rtt_ms`
* `p2_summary.csv`
  Résumé d’exécution : transport, rxBytes, goodputbps, etc.
* `flowmon.xml` (si `--flowmon=true`)
* `*.pcap` (si `--pcap=true`) : captures Wi-Fi + CSMA

### 8.2 `logs/`

* `console_<transport>_run<run>.txt` : sortie console (si lancement via script `run_p2.sh` ou `tee`)

### 8.3 `plots/`

* Figures produites par `plot_p2.py` (PNG/PDF)

---

## 9) Définition du Goodput (obligatoire)

* `rxBytes = PacketSink::GetTotalRx()`
* Durée utile : `Tutile = simTime - appStart`
* `goodputbps = 8 * rxBytes / Tutile`

Cette définition évite de sous-estimer le goodput en moyennant sur toute la durée (incluant le warm-up).

---

## 10) RTT (note sur V4Ping)

Selon la configuration/build ns-3, `V4Ping` peut être indisponible (en-têtes/helpers absents).
Dans ce Part, le RTT est mesuré via une **sonde UDP echo horodatée** :

* un serveur UDP “echo” sur `serverNode`,
* un client UDP sur `staNode` qui envoie un paquet avec `seq + timestamp`,
* calcul du RTT au retour,
* export dans `rtt_timeseries.csv`,
* affichage console optionnel via `--rttVerbose=true`.

---

## 11) Post-traitement (plots)

Script :

* `~/wifi-ter-sim/scripts/plot_p2.py`

Exécution (par défaut : UDP run1 et TCP run2) :

```bash
chmod +x ~/wifi-ter-sim/scripts/plot_p2.py
~/wifi-ter-sim/scripts/plot_p2.py --format=png --cdf
```

Sorties (dans `plots/` de chaque run) :

* `throughput_vs_time_<label>.png|pdf`
* `rtt_vs_time_<label>.png|pdf`
* `rtt_cdf_<label>.png|pdf` (si `--cdf`)
* `stats_<label>.txt`

---

## 12) Vérifications rapides

Après un run :

```bash
ls -lh ~/wifi-ter-sim/results/p2/udp/run1/raw
ls -lh ~/wifi-ter-sim/results/p2/tcp/run2/raw
```

Fichiers attendus :

* `throughput_timeseries.csv`
* `rtt_timeseries.csv`
* `p2_summary.csv`
* `flowmon.xml` (si activé)
* `*.pcap` (si activé)

---

## 13) Dépannage

### 13.1 Problèmes d’arguments avec `ns3 run`

Utiliser la forme suivante (sans séparateur problématique) :

```bash
./ns3 run "scratch/p2_baseline --transport=udp ..."
```

### 13.2 Écrasement des sorties

Ne pas réutiliser le même `--outDir` pour plusieurs runs/transport. Utiliser des sous-dossiers :

* `.../udp/run1`
* `.../tcp/run2`
  ou lancer `scripts/run_p2.sh`.

### 13.3 PCAP trop volumineux

Désactiver PCAP :

```bash
--pcap=false
```

---

## 14) Livrables minimaux

* Code : `scenarios/p2_baseline.cc`
* Scripts :

  * `scripts/run_p2.sh`
  * `scripts/plot_p2.py`
* Résultats :

  * CSV (goodput/RTT), `p2_summary.csv`
  * Figures (plots)
  * `flowmon.xml` (si demandé)
  * PCAP (optionnel selon consignes)

```
