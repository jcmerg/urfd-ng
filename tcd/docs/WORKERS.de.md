# tcd-Modul-Worker

[English](WORKERS.md) | **Deutsch**

Wie tcd die Module auf die CPU-Kerne verteilt und was es tut, wenn ein Worker oder ein AMBE-Gerät nicht hinterherkommt. Die Einstellung selbst, `Workers`, erklärt die [README](../README.de.md#modul-worker).

## Inhalt

- [Worker und Module](#worker-und-module)
- [Ein Worker kommt nicht hinterher](#ein-worker-kommt-nicht-hinterher)
- [AMBE-Geräte](#ambe-geräte)
- [Codec-Plugins](#codec-plugins)
- [Der Bericht an urfd](#der-bericht-an-urfd)

## Worker und Module

Jedes Modul gehört zu einem Worker-Thread. Der Worker erledigt alles für die Frames seines Moduls, einen Codec nach dem anderen: Decodieren, AGC, Codieren in die anderen Codecs in Software, die Antworten der AMBE-Geräte und das Zurücksenden an urfd. Nur der Worker des Moduls fasst einen Frame an, die Codecs eines Frames brauchen also keine Locks untereinander; die Worker verschiedener Module laufen gleichzeitig.

`Workers = auto` nimmt einen Worker je CPU-Kern, den tcd nutzen darf: die Kerne seiner Affinitätsmaske (`docker --cpuset-cpus`) und höchstens die CPU-Quote seiner cgroup, aufgerundet (`docker --cpus`, systemd `CPUQuota=`), nie mehr als einen je Modul. Die Module werden der Reihe nach in der Reihenfolge von `Modules` und `StandbyModules` verteilt, bei 9 Modulen auf 8 Kernen teilen sich also das erste und das neunte einen Worker. Das Log nennt die Aufteilung beim Start: `8 module workers: FR S A C D H N P`.

Der Zustand, der zu einem Stream gehört (die Zustände der Codec-Plugins, die AGC, der b2-Gain, die Rauschunterdrückung, das Stream-Log), liegt je Modul, jeweils mit eigenem Lock, so warten die Worker nicht aufeinander. Die AMBE-Geräte haben einen eigenen Lock, den ein Worker nur nimmt, wo ein Gerät beteiligt ist.

## Ein Worker kommt nicht hinterher

Ein Worker kann in Rückstand geraten, wenn ein Codec-Plugin hängt, das Senden eines Moduls blockiert oder der Rechner überlastet ist. Ein Paket von urfd, das länger als 400 ms auf seinen Worker gewartet hat, wird dann verworfen: So spät nützt es urfd nichts mehr, und der Worker holt auf. Nie verworfen werden:

- der erste Frame eines Streams: Die direkte Transcodierung beginnt nur dort
- der letzte Frame eines Streams: Mit ihm enden die Codec-Zustände, der AMBE-Kanal und das Stream-Log
- die Antwort eines AMBE-Geräts: Dieser Frame ist fertig

Die Queue eines Workers fasst höchstens 2000 Pakete, als Grenze für den Speicher. Das Log meldet höchstens einmal pro Sekunde, dass ein Worker Frames verwirft. Web-Monitor und `tcdmon` zeigen für jeden Worker seine Auslastung (den Anteil eines CPU-Kerns, den er in der letzten Sekunde gearbeitet hat) und die längste Wartezeit eines Frames auf ihn; rot ab 70 % oder 20 ms. Das Warten auf ein Codec-Plugin ohne parallele Aufrufe zählt dort als Arbeit.

M17 codiert eine Payload in zwei Paketen, das zweite mit der Hälfte des ersten. Ein zweites Paket, dessen erstes nicht ankam (verworfen oder schon vor urfd verloren), wird gegen Stille codiert, nicht gegen die Hälfte einer älteren Payload.

## AMBE-Geräte

Die Geräte behalten ihre eigenen Threads für die serielle Leitung: einen Feeder, der die Frames sendet, und einen Reader, der die Antworten abholt. Ein Worker übergibt dem Gerät einen Frame und macht derweil mit anderen Frames weiter; die Antwort geht an den Worker des Moduls zurück.

- **FIFO**: Der Feeder sendet nie einen Frame in einen vollen FIFO des Chips (5 Frames beim DV3003, 2 beim DV3000), denn der Chip würde ihn verwerfen, und die nächste Antwort ginge an den falschen Frame.
- **Zu spät für das Gerät**: Ein Frame, der eine Sekunde auf sein Gerät gewartet hat, geht mit Stille weiter, so staut ein Gerät, das langsamer ist als die Frames kommen (aber noch antwortet), sie nicht ohne Ende auf.
- **Verlorene Antworten**: Eine Antwort, die das Gerät 300 ms schuldig bleibt, gilt als verloren (siehe [Verlorene Antworten](../README.de.md#verlorene-antworten-devicecompand)); ihr Frame geht mit Stille weiter.
- **Ein hängendes oder verlorenes Gerät**: Ein Gerät, das 1 s lang gar nicht antwortet, obwohl Frames warten, oder das abgezogen wird, wird herausgenommen (siehe [Ein Gerät fällt im Betrieb aus](../README.de.md#ein-gerät-fällt-im-betrieb-aus)). Die Frames, die es bekommen hat, und die noch wartenden gehen sofort mit Stille weiter, ebenso Frames, die ihm übergeben werden, während es geschlossen ist (ausgephast, für ein anderes `DeviceCompand` neu geöffnet).
- **Reihenfolge**: Die Stille eines verlorenen oder geschlossenen Geräts geht in der Reihenfolge hinaus, zuerst die Frames, die es bekommen hat, dann die noch nicht gesendeten. urfd nimmt die Frames eines Streams nach ihrer Nummer und verwirft einen, der nach einem neueren kommt.
- **Kanal-Pool**: Der AMBE-Kanal eines Streams wird erst frei, wenn sein letzter Frame beim Gerät eingereiht ist, so kann der erste Frame eines anderen Moduls nicht vor ihm landen.

## Codec-Plugins

Die eigenen Vocoder von tcd (OP25, Codec2) werden parallel aufgerufen. Ein Codec-Plugin dann, wenn es `TCD_PLUGIN_CONCURRENT` setzt (Codec-API 5): Dann rufen die Worker es gleichzeitig für die Zustände verschiedener Streams auf, `open` und `close` eingeschlossen, nie zweimal zugleich für denselben Zustand. Ein Plugin ohne das Flag (API 2 bis 4) ruft immer nur ein Worker zur Zeit auf; das Log sagt das beim Start. Siehe [Ein Plugin schreiben](../README.de.md#ein-plugin-schreiben) und [`CodecPlugin.h`](../CodecPlugin.h).

## Der Bericht an urfd

Nach einem Durchgang schickt tcd urfd einen Bericht über dessen Audio (Pegel, AGC-Gain, FEC). Er geht hinaus, sobald der letzte Frame des Streams codiert und zurückgeschickt ist, gleich worauf er gewartet hat (einen Worker, ein Gerät, eine verlorene Antwort), so stecken die Frames davor darin. Ein Stream, dessen letzter Frame verloren ging, wird 2 s nach seiner letzten Aktivität berichtet.
