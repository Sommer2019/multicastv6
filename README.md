# multicastv6 — Video Roundsend über IPv6 Multicast (C++)

Diese Version stellt eine reine C++-Implementierung (Sender + Receiver) bereit, die auf Debian-Systemen einfach gebaut werden kann.

Enthaltene Dateien
- src/sender.cpp   — C++ IPv6 Multicast Sender
- src/receiver.cpp — C++ IPv6 Multicast Receiver
- Makefile         — Build / install targets

Voraussetzungen (Debian)
- build-essential (g++, make)
  sudo apt update
  sudo apt install -y build-essential

Build
1. Im Repository-Verzeichnis:
   make

   Das erzeugt zwei Binaries im aktuellen Verzeichnis:
   - ./sender
   - ./receiver

Install (optional)
   sudo make install
Das installiert Binaries nach /usr/local/bin (ändere PREFIX falls gewünscht).

Verwendung / Beispiele
- Sender (Server):
  ./sender -f input.mp4 -a ff3e::1 -p 12345 -i eth0 -r 800

  Parameter:
  - -f, --file    : Pfad zur zu sendenden Datei (erforderlich)
  - -a, --addr    : IPv6 Multicast-Adresse (default ff3e::1)
  - -p, --port    : UDP Port (default 12345)
  - -i, --iface   : Interface-Name (bei link-local ff02:: Adressen erforderlich)
  - -r, --pps     : Pakete pro Sekunde (0 = so schnell wie möglich)

- Receiver (Client):
  ./receiver -o out.mp4 -a ff3e::1 -p 12345 -i eth0

  Parameter:
  - -o, --out     : Ausgabedatei ('-' für stdout)
  - -t, --timeout : Sekunden warten auf fehlende Pakete nach Finalmarker (default 10)

Playback während Empfang
- Empfänger schreibt auf stdout und pipe zu ffplay:
  ./receiver -o - -a ff3e::1 -p 12345 -i eth0 | ffplay -i -

Hinweise / Troubleshooting
- Link-local Adressen (ff02::/16) benötigen die Angabe der Schnittstelle (-i).
- Prüfe Gruppenmitgliedschaft:
  ip -6 maddr show dev eth0
- Beobachte Netzverkehr:
  sudo tcpdump -n -i eth0 'ip6 and udp and port 12345'
- Firewall:
  sudo ip6tables -A INPUT -p udp --dport 12345 -j ACCEPT

Protokollformat
- Pro Paket wird ein 8-Byte Header gesendet:
  - 4 bytes: sequence (big-endian uint32)
  - 4 bytes: flags    (big-endian uint32) -> bit 0 = final
- Payload pro UDP-Paket: 1200 bytes
- Receiver puffert Out‑of‑Order Pakete und schreibt in Reihenfolge.
- Keine Retransmission oder FEC; geeignet für kontrollierte LAN-Umgebungen.
```
