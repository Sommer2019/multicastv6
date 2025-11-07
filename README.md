# multicastv6 — Video Roundsend über IPv6 Multicast (Go + Python)

Dieses Repository enthält mehrere Varianten eines einfachen Multicast "roundsend" Werkzeugs (Sender + Receiver), mit dem ein Rechner eine Videodatei gleichzeitig an viele Empfänger per Multicast senden kann.

Enthaltene Implementierungen

- Go (leichtgewichtig, bereits vorhanden): sender.go, receiver.go — sendet und empfängt Dateien über IPv6 Multicast mit einfachem Sequenz-Header.
- Python (Debian-freundlich, neu hinzugefügt): sender.py, receiver.py — dieselbe Header-Struktur (4 Byte Seq, 4 Byte Flags), ausführbar auf Debian ohne zusätzliche Pakete.

Python Schnellstart (Beispiele)

1) Sender (Server):
   python3 sender.py -f input.mp4 -a ff3e::1 -p 12345 -i eth0 -r 800

2) Receiver (Client):
   python3 receiver.py -o out.mp4 -a ff3e::1 -p 12345 -i eth0

3) Playback während Empfang (Pipe):
   python3 receiver.py -o - -a ff3e::1 -p 12345 -i eth0 | ffplay -i -

Wichtige Hinweise
- UDP Multicast ist unzuverlässig: Paketverlust, Reordering und Jitter sind möglich. Diese Implementierungen bieten nur einfache Reordering-Pufferung.
- Link-local Adressen (ff02::/16) benötigen die Angabe der Schnittstelle (-i/--iface).
- Für produktive Streaming-Szenarien empfehlen sich RTP/RTCP + FEC oder ffmpeg/gstreamer.

Troubleshooting
- Prüfe Gruppenmitgliedschaft auf dem Empfänger: `ip -6 maddr show dev eth0`
- Siehe Netzwerkverkehr: `sudo tcpdump -n -i eth0 'ip6 and udp and port 12345'`
- Firewall: `sudo ip6tables -A INPUT -p udp --dport 12345 -j ACCEPT`

