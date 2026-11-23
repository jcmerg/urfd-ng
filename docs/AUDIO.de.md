# Audio

[English](AUDIO.md) | **Deutsch**

Was urfd und tcd mit der Sprache machen, die durch den Reflektor geht, wie das Dashboard einen Durchgang bewertet, und die Messungen dahinter. Die eigenen Einstellungen des Transcoders (AGC, Verstärkungen, Vocoder) stehen in [tcd/README.de.md](../tcd/README.de.md).

## Gleicher Pegel in allen Betriebsarten

Die AGC von tcd bringt jeden Ruf auf einem transcodierten Modul auf denselben Pegel, egal von welchem Funkgerät oder Vocoder er kommt. Das gilt auch für eine Betriebsart, die in sich selbst weitergeht (DMR → DMR/YSF, D-Star → D-Star, P25 → P25, M17 → M17), wo die Frames ohne Vocoder durchgehen: Die Verstärkung der AGC geht in den Verstärkungsparameter des Frames (AMBE- und IMBE-b2, die Codec2-Energie) statt in ein Dekodieren und Neukodieren, der Klang bleibt also so sauber wie das Original.

Die AGC folgt der oberen Hüllkurve der Sprache und hebt nur bei deutlich stimmhafter Sprache weit über einem Rauschboden an, den sie je Stream misst, so pumpt die Verstärkung nicht bei Atmen, Rauschen oder Pausen. Bei Verkehr aus dem FM-Funknetz bewegte sich die Verstärkung damit nur ein Drittel so stark wie bei einer reinen Pegel-AGC, und die Sprache stand 2,5 dB weiter über dem Rauschen, bei gleichem Pegel und gleicher Streuung zwischen den Stationen.

SvxLink und AllStar (SVX, USRP) laufen durch eine eigene AGC für analoge Quellen: FM-Audio ist zwischen den Worten nicht still, sondern trägt das Rauschen des Empfängers, das die AGC für digitale Quellen um bis zu 30 dB anhob. Bei 20 Minuten Verkehr aus dem FM-Funknetz hielt die analoge AGC 32 dB Abstand zwischen Sprache und Rauschen, gegenüber 19 dB mit der digitalen.

Zwei DVSI-Geräte, z.B. DV3000 + DV3003 für zwei Module, brauchen kein Codec-Plugin; ein Raspberry Pi 4 reicht locker.

## FM-Pegel von SvxLink und AllStar

Der Empfänger des Knotens darf nicht übersteuern, der AMBE-Encoder lässt übersteuertes Audio rau klingen. Ein SHARI-Knoten (CM108) mit `Mic Capture Volume` auf 23 von 35 übersteuerte jede Aussendung (Sprache bei -6 dBFS, Spitzen bei Vollaussteuerung); auf 15 kam die Sprache mit -16 dBFS ohne Übersteuerung:

```bash
amixer -c Device cset numid=8 15
alsactl store
```

`RxGain` in der `urfd.ini` auf 0 lassen und die analoge AGC von tcd den Pegel einstellen lassen. Das FM-Rauschen unter der Sprache klingt nach dem AMBE-Encoder auf DMR roboterhaft; tcd entfernt das meiste davon vor dem Kodieren (`AGCAnalogNoiseSuppress`).

## Verlorene Sprachframes aufgefüllt

D-Star nummeriert seine Sprachframes 0..20 und DMR seine Bursts A..F mit je drei Frames. Gehen auf dem Weg zum Reflektor bis zu 6 Frames verloren, setzt urfd an ihre Stelle Frames mit dem Verlust-Code des jeweiligen Vocoders (D-Star b0 127, AMBE+2 b0 120 als Erasure, samt FEC). Jeder Decoder, die Codec-Plugins von tcd, die DVSI-Chips und die Funkgeräte, wiederholt für sie den letzten Frame und schaltet nach dreien stumm, wie bei einem Frame, den er nicht korrigieren konnte, und der Stream behält seine Frame-Zählung, D-Star-Sync und DMR-Bursts. Ohne das hörten die Decoder den nächsten Frame zu früh, und D-Star-Empfänger verlören die Sync-Position.

- Die aufgefüllten Frames gehen sofort hinaus, vor dem Frame, der die Lücke zeigte, die Verzögerung des Streams wächst also um die Lücke.
- Ein Frame bis zu 6 Positionen zurück, der innerhalb von 0,2 s kommt, ist verspätet oder wiederholt und wird verworfen; seine Position ist schon hinaus. Tiefere Umsortierung lässt sich von einem Verlust nicht unterscheiden.
- Längere Lücken bleiben, wie sie sind. XLX-Partner (D-Star und DMR in einem Frame) und die Wiedergabe des Echo-Moduls werden nicht aufgefüllt.
- DMR+ nutzt eine Stream-ID je Adresse, so schließt sich eine neue Aussendung wenige Frames nach einem verlorenen Endpaket dem alten Stream an, mit bis zu 6 aufgefüllten Frames dazwischen.
- Das Log meldet beim Schließen eines Streams `Module A: 5 lost voice frames filled in`.
- P25, NXDN, YSF, USRP und M17 haben keinen Frame-Zähler, den urfd nutzen könnte (SVX verdeckt seine Lücken selbst).

## Wiederholte AMBE+2-Frames stummgeschaltet

Manche Funkgeräte und Hotspots beenden eine DMR-, YSF- oder NXDN-Aussendung mit demselben AMBE+2-Frame immer wieder, 6- bis 30-mal. Auf BrandMeister TG 262 und 91 war das ein Drittel der Aussendungen, von einem Drittel der Stationen, und jeder Decoder spielte ihn als letzten Laut noch einmal. Sprache hat nie zweimal hintereinander denselben Frame, außer dem Stille-Frame, deshalb sendet urfd ab dem dritten in Folge stattdessen AMBE+2-Stille, für jedes Ziel; das Log zählt sie je Stream.

## Echo vom Ende eines Rufs vom Master

BrandMeister (TG 262, von vielen Stationen) schickt nach den meisten Rufen 0,6 bis 1,2 s später einen zweiten Stream derselben ID mit den letzten 36 bis 66 Frames des Rufs noch einmal (je Frame ein, zwei Bit anders), wie über einen zweiten, verzögerten Weg; alle hörten die letzte Sekunde doppelt. Die MMDVMClient-Anbindung hält einen neuen Stream einer ID, die in den 3 s davor auf dem Modul sprach, bis zu seinen ersten Sprachframes zurück (meist 60 ms, höchstens 360 ms). Sind es Frames aus dem Ruf dieser ID der letzten 3 Sekunden, verwirft sie den Stream und protokolliert `echo of the end of the call from ... dropped`. Andere IDs, die dazwischen tasten, stören die Prüfung nicht. Ein neuer Ruf dieser ID mit anderer Sprache geht durch. Geprüft an einer Stunde BrandMeister TG 262: alle 33 Echos verworfen, kein anderer Stream.

## DMR-Rufe ohne Sprache

Manche Geräte tasten nach einem Sprechgruppenwechsel von selbst: eine ganze Aussendung von etwa 3 s, deren Sprachframes nur aus Null-Bytes bestehen. Auf `[MMDVM]` öffnet ein Ruf seinen Stream erst mit dem ersten Sprachframe, der nicht nur aus Nullen besteht; einer ohne wird verworfen und protokolliert (`call from ... without voice ... ignored`), so erscheint er weder in Last heard noch als 3 s Nichts auf dem Modul.

## Vom Transcoder verlorene Frames

urfd ordnet jede Antwort von tcd dem Frame zu, auf den es wartet. Beantwortet tcd einen Frame nie (ein AMBE-Gerät hing), werden die Frames vor der nächsten Antwort übersprungen und die Aussendung geht weiter, statt ihren Rest zu verlieren.

## Latenz von YSF VW

VW-Sprache (Voice Wide, IMBE Full Rate) erreicht die anderen Betriebsarten etwa 85 ms später als DN: Der erste VW-Frame nach dem Header hat 40 ms Sprache.

## Klangbericht zu jedem Durchgang

Auf einem transcodierten Modul meldet tcd nach jedem Durchgang, wie er klang: Eingangspegel und Spitze vor der AGC, den Anteil der Sprachframes mit einer Spitze von -1 dBFS oder mehr (übersteuert), den Pegel nach der AGC und die Verstärkung der AGC, bei AMBE-Streams die Frames, die die FEC korrigierte oder nicht retten konnte, und mit einem Codec-Plugin als Decoder die Frames, die es wiederholte oder stummschaltete. urfd ergänzt die auf dem Weg verlorenen und aufgefüllten Sprachframes und die Länge des Durchgangs (auf jedem Modul). Er wird mit dem Last-Heard-Eintrag gespeichert und als `user_report` an das Dashboard geschickt ([docs/WEBSOCKET.md](WEBSOCKET.md)). Nichts darin nennt den Vocoder.

## Wie das Dashboard einen Durchgang bewertet

Last heard zeigt auf transcodierten Modulen ein kleines Messgerät des Eingangspegels mit seiner Spitze, ein Wort in Grün, Orange oder Rot, was die AGC geändert hat, und die verlorenen oder stummgeschalteten Frames; die Einzelheiten stehen im Tooltip.

| Wort | Wann |
|------|------|
| Short (grau) | unter 1,5 s oder unter 1 s Sprache: zu kurz, um den Pegel zu bewerten |
| No speech | 3 s oder mehr ganz ohne Sprache (klemmende PTT, totes Mikrofon) |
| Overdriven | 5 % oder mehr der Sprache übersteuert, oder Pegel über -8 dBFS |
| Too quiet | Pegel unter -46 dBFS |
| Loud | 1 % oder mehr übersteuert, oder Pegel über -12 dBFS |
| Quiet | Pegel unter -36 dBFS |
| Bit errors, Packet loss | 2 % (orange) oder 5 % (rot) der Frames an die FEC oder das Netz verloren, gezählt ab 50 Frames |
| Good | sonst |

Die Pegelbereiche folgen den Codec-Tests von 3GPP, die -26 dBov als nominal und -16 bis -36 dBov als normal nehmen; „Too quiet“ beginnt 20 dB unter nominal, wo die AGC an ihre Grenze kommt. DMR-Geräte kommen häufig um -32 dBFS an, was die AGC ohne Mühe anhebt.

**Hide kerchunks** blendet beendete Durchgänge unter 1 s aus (nach der Länge bei urfd, Header bis letztes Paket), und längere mit unter 0,3 s Sprache, außer Bitfehler oder Paketverlust haben sie stummgeschaltet. Ein „73“ von 1 s oder mehr bleibt, ebenso „No speech“; eine Station auf Sendung erscheint immer. Die Wahl merkt sich der Browser.
