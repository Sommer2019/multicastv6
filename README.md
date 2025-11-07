# multicastv6 — Video Roundsend über IPv6 Multicast (C++ mit Stream‑ID)

Diese Version erlaubt mehrere Sender in derselben Multicast‑Gruppe/Port, indem jedem Stream eine stream_id zugewiesen wird. Receiver können sich gezielt für einen oder mehrere Streams entscheiden.

Build Voraussetzungen (Debian)
- build-essential:
  sudo apt update
  sudo apt install -y build-essential

Build
1. Im Repo‑Root:
   make

Das erzeugt:
- ./sender
- ./receiver

Usage — Sender
- Beispiel: Sender mit stream_id 42
  ./sender -f input.mp4 -S 42 -a ff3e::1 -p 12345 -i eth0 -r 800

Argumente:
- -f, --file       : Pfad zur Datei (erforderlich)
- -S, --stream-id  : stream_id (uint32, default 1)
- -a, --addr       : IPv6 Multicast Adresse (default ff3e::1)
- -p, --port       : UDP Port (default 12345)
- -i, --iface      : Interface Name (bei link-local Adressen erforderlich)
- -r, --pps        : Pakete pro Sekunde (0 = so schnell wie möglich)

Usage — Receiver
- Subscribe zu einem Stream:
  ./receiver -s 42 -o out_{id}.mp4 -a ff3e::1 -p 12345 -i eth0

- Subscribe zu mehreren Streams:
  ./receiver -s 42,43 -o stream_{id}.mp4 -a ff3e::1 -p 12345 -i eth0

- Subscribe zu allen Streams (Dynamic):
  ./receiver -s all -o stream_{id}.mp4 -a ff3e::1 -p 12345 -i eth0

- Direct playback for single stream to ffplay:
  ./receiver -s 42 -o - -a ff3e::1 -p 12345 -i eth0 | ffplay -i -

Parameter:
- -s, --subscribe  : "all" oder kommagetrennte Liste von stream_ids
- -o, --out        : Output pattern, benutzen Sie "{id}" als Platzhalter (z.B. "out_{id}.mp4"), oder "-" für stdout wenn nur ein Stream abonniert
- -t, --timeout    : Sekunden warten auf fehlende Pakete nach Finalmarker (default 10)

Beispiele — Multi‑Sender/All‑to‑All
- Jeder Host wählt eine eindeutige stream_id (z. B. Hostnummer) und sendet:
  ./sender -f hostA.mp4 -S 101 -a ff3e::1 -p 12345 -i eth0

- Ein Empfänger, der alle Streams empfangen will:
  ./receiver -s all -o stream_{id}.mp4 -a ff3e::1 -p 12345 -i eth0

Wichtige Hinweise
- Stream‑ID Einzigartigkeit: Wenn zwei Sender dieselbe stream_id nutzen, mischen sich ihre Pakete => Kaputtes Ergebnis.
- Netz: Multicast muss im LAN erlaubt sein; bei Link‑Local Adressen (-a ff02::...) muss -i gesetzt werden.
- UDP bleibt unzuverlässig: kein Retransmission/FEC in dieser Implementierung.
