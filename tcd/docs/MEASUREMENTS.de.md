# Messungen zu tcd

[English](MEASUREMENTS.md) | **Deutsch**

Die Messungen und Hörtests hinter den Voreinstellungen von tcd, für alle, die wissen wollen, warum eine Einstellung so ist, wie sie ist, oder sie abstimmen möchten. Die Einstellungen selbst erklärt die [README](../README.de.md).

## Inhalt

- [DMR-Pegel](#dmr-pegel)
- [OutputGainDMRBitstream](#outputgaindmrbitstream)
- [Genauigkeit des b2-Gains (`DmrGainMode = ambe`)](#genauigkeit-des-b2-gains-dmrgainmode--ambe)
- [AGC bei analogen Quellen](#agc-bei-analogen-quellen)
- [Rauschunterdrückung](#rauschunterdrückung)
- [Sprachgesteuerte AGC](#sprachgesteuerte-agc)
- [Verlorene Antworten am DV3003 und `DeviceCompand`](#verlorene-antworten-am-dv3003-und-devicecompand)
- [Direkte Transcodierung](#direkte-transcodierung)
- [P25 nach M17](#p25-nach-m17)
- [CPU-Last und Latenz](#cpu-last-und-latenz)

## DMR-Pegel

DMR-Funkgeräte erwarten einen niedrigeren Pegel als das AGC-Ziel von -16 dBFS. Sprache auf BrandMeister, gemessen auf TG91: Median -29 dBFS, -39 bis -23 dBFS zwischen den Sprechern. `OutputGainDMR = -14` bringt jeden Weg nach DMR/YSF auf etwa -30 dBFS, passend dazu; -18 ließ DMR/YSF etwa 4 dB darunter. Der Versatz gehört in `OutputGainDMR`, nicht in `AmbeGainDb`/`DmrReencodeGain`: Die wirken nur auf DMR → DMR/YSF, mit -18 dort war D-Star → DMR 18 dB zu laut.

## OutputGainDMRBitstream

Mit `OutputGainDMR = -14` als PCM-Verstärkung bekommt der AMBE2+-Encoder das Audio der anderen Codecs mit etwa -34 dBFS, weit unter dem Pegel, für den er gemacht ist. `OutputGainDMRBitstream` verschiebt einen Teil der Verstärkung hinter den Encoder, in den Gain-Parameter b2 des Frames.

- **Alles in b2** klang über einen SvxLink-Knoten nach DMR (2026-10-15) etwas besser, aber hier und da mit Artefakten: b2 bewegt leise Frames in groben Stufen von 3-16 dB, eine große Verschiebung lässt sich auf leisen Silben nicht treffen, und der Fehler wird in den folgenden Frames ausgeglichen. Darum eine Aufteilung in dB: -9 etwa gibt dem Encoder -25 statt -34 dBFS und b2 nur die halbe Verschiebung.
- **Knistern bei -6**: Zwischen zwei groben b2-Stufen glich der Nachführer den Rundungsfehler im nächsten Frame aus, so sprang der Gain leiser Frames alle 20 ms um 3-6 dB hin und her. Der Gain dieses Wegs bewegt sich jetzt um höchstens 1,2 dB je Frame; in einer Simulation mit sprachähnlichen b2-Folgen bei -4 dB bleibt kein Sprung über 3 dB (vorher 68), und laute Frames treffen das Ziel weiterhin. Damit klangen -4 und -6 über SvxLink nach DMR gleich und sauber (2026-10-15); -6 wurde der Standard. DMR → DMR behält den unbegrenzten Nachführer, er war in Tests sauber.
- **b2-Wahl mit Vorausschau**, simuliert und verworfen (2026-10-15): Eine Strahlsuche über die nächsten Frames mit quadratischen Kosten auf Gain-Änderungen beseitigt die Sprünge über 3 dB bei -6 (61 → 0) erst mit 6 Frames Vorausschau, etwa 100 ms, und erreicht etwa -8 dB mit so vielen Sprüngen wie die einfache Grenze bei -6; ohne Vorausschau verfehlt sie das Ziel um etwa 3 dB. 2 dB mehr Pegel für den Encoder sind 60-100 ms mehr Verzögerung auf jedem Frame nicht wert (tcd sendet ein Paket, wenn alle Codecs fertig sind), zumal -4 und -6 schon sauber klingen: Die meisten gezählten Sprünge liegen an Silbengrenzen und werden verdeckt. Die b2-Tabelle selbst ist Teil von AMBE2+ und in jedem Decoder fest.

## Genauigkeit des b2-Gains (`DmrGainMode = ambe`)

b2 um eine feste Zahl Stufen zu verschieben änderte leises Audio um 3-16 dB je Stufe und lautes um etwa 1,2 dB, der Gain hing also vom Pegel ab. tcd verfolgt jetzt γ für den ursprünglichen und den veränderten Stream und wählt das b2, das dem gewünschten Gain am nächsten kommt. Gemessen mit einem AMBE2+-Software-Decoder bei Eingang mit -40, -30 und -16 dBFS: Gains von -12 bis +6 dB kommen auf 0,8 dB genau an.

## AGC bei analogen Quellen

FM-Audio ist zwischen den Wörtern nicht still, sondern trägt das Rauschen des Empfängers, oft -45 bis -35 dBFS, bei dem das Gate für digitale Quellen nie schließt.

- **Simulation** über `AGC.h`, ein gutes FM-Signal (Rauschen bei -45 dBFS, Sprache bei -28 dBFS): Mit dem digitalen Profil fiel der Abstand zwischen Sprache und Rauschen von 17 auf 11 dB, die AGC hob das Rauschen zwischen den Wörtern um bis zu 30 dB an. Mit den Standards des analogen Profils steigt er auf 25 dB, und verrauschtere Signale werden nicht mehr schlechter.
- **20 Minuten FM-Funknetz-Verkehr**, aufgezeichnet an einem SvxLink-Knoten: Bei HF-Stationen fiel der Abstand mit dem digitalen Profil von 34 auf 19 dB und bleibt mit dem analogen bei 32 dB; bei App- und RoIP-Teilnehmern 36 dB (19,7 mit dem digitalen Profil).

## Rauschunterdrückung

QSOs über einen SvxLink-Knoten nach DMR (2026-10-15): Bei -15 dB Rauschunterdrückung, zusätzlich mit Gate-Boden -12 dB, waren die Pausen totenstill, das Komfortrauschen fehlte, und leise Silbenanfänge und -enden klangen abgeschnitten, weil speexdsp sie für Rauschen hält. -10 dB mit Gate-Boden 0 lassen ein leises, natürliches Grundrauschen und ganze Silben, und der roboterhafte AMBE-Klang auf DMR bleibt weg. Stärkere Werte entfernen mehr Rauschen, schneiden aber Silben ab und hinterlassen „musikalische“ Artefakte.

Die DVSI-Chips haben eine eigene Rauschunterdrückung (`NS_ENABLE` in `ECMODE`, standardmäßig an); tcd schaltet sie ab (`ECMODE = 0`). Sie sitzt im Encoder-Kanal und würde auf alles wirken, was der Kanal codiert, auch sauberes D-Star-, M17- und P25-Audio, kann eine FM-Quelle nicht von den anderen unterscheiden und wird je Kanal beim Start gesetzt, nicht je Stream. speexdsp wirkt dort, wo der analoge Eingang hereinkommt, nur bei SVX und USRP, für jeden Ziel-Codec gleich.

## Sprachgesteuerte AGC

Die AGC verfolgt den Pegel in dB auf der oberen Hüllkurve der Sprache und hebt den Gain nur auf deutlich stimmhaften Frames klar über einem je Stream gemessenen Grundrauschen an. Gemessen an 18 FM-Funknetz-Durchgängen von HF-Stationen (2026-10-23) über den analogen Weg (Rauschunterdrückung und AGC analog) gegen die frühere AGC mit gleitendem RMS-Fenster:

- die Stationen liegen so dicht beieinander wie vorher (Streuung 1,2 dB), bei etwa demselben Pegel (-16,4 gegen -15,6 dBFS)
- der Gain bewegt sich von Frame zu Frame ein Drittel so viel (0,20 gegen 0,55 dB)
- die Sprache steht 2,5 dB weiter über dem Rauschen in den Pausen

## Verlorene Antworten am DV3003 und `DeviceCompand`

Raspberry Pi 5 mit einem DVstick-33 bei 921600 Baud (2026-11-02): eine Antwort von etwa 30 000 bis 60 000 durch einen FTDI-Empfangsüberlauf verloren, etwa 20 in 1,5 Stunden Verkehr auf einem Reflektor im Betrieb; nie beim DV3000 mit 460800 Baud. Betroffen waren nur Antworten von Decodern (je 327 Byte Audio); Antworten von Encodern sind 16 Byte lang.

Mit `DeviceCompand` (8-Bit-G.711 statt 16 Bit linear, halb so viele Bytes): 0 Überläufe in 20 Minuten der Last, die vorher etwa 7 hatte. Audioqualität (PESQ), linear / µ-law / A-law:

| Weg | linear | µ-law | A-law |
|---|---|---|---|
| ein auf dem Chip decodierter DMR-Clip | 2,811 | 2,807 | 2,800 |
| durch tcd, DMR hinein, sein PCM heraus | 2,946 | 2,942 | 2,946 |
| DMR → D-Star (auf den Chips decodiert und codiert) | 2,207 | 2,180 | 2,175 |

Hinter dem AMBE-Codec ist der Unterschied nicht zu hören. Ein Byte, das dann doch verloren geht, verschiebt die Samples eines Frames um ein Sample (eine achtel Millisekunde), statt sie zu Rauschen zu machen.

## Direkte Transcodierung

D-Star, DMR/YSF und P25 teilen das MBE-Vocoder-Modell; die zweite Analyse synthetisierter Sprache ist der größte Verlust eines Transcoders. Den Ziel-Frame aus den Parametern des Quell-Frames zu codieren (`Transcode = direct`), über 19 Clips gemessen: Jede der sechs Richtungen gewinnt 0,09 bis 0,24 PESQ gegenüber Decodieren und Encodieren, für D-Star und DMR durch einen DV3000 bestätigt, bei einem Fünftel der Rechenzeit.

## P25 nach M17

Der IMBE-Decoder legt zusätzliche Energie unter 120 Hz in sein Audio, das brachte die Grundfrequenz-Schätzung von Codec2 durcheinander: PESQ etwa 1,6. Ein 300-Hz-Hochpass (4. Ordnung) vor dem Codec2-Encoder für P25-Audio bringt es auf 2,5.

## CPU-Last und Latenz

- Mit `-O2` gebaut braucht ein transcodierter Stream (Codec2, IMBE und AGC in Software) etwa 7 % eines Kerns auf einem Raspberry Pi 4 und 3 % auf einem Pi 5.
- Der eigene P25-Vocoder von tcd (OP25) mit Link-Time-Optimierung, `-O3` und einer 64-Bit-Grundfrequenzsuche, auf einem Raspberry Pi 4: Codieren 0,37 ms je Frame (vorher 1,06), Decodieren 0,15 ms (0,27), bitgleiche Frames. Ein Pi 4 schafft 26 Module, die gleichzeitig P25 sprechen (99 % der Frames innerhalb von 15 ms); vorher fiel er bei 18 zurück.
- `TCP_NODELAY` auf der Verbindung zum Reflektor: Ein Frame kommt nach etwa 23 ms statt 60 ms zurück.
- Modul-Worker, auf einem Raspberry Pi 5 (4 Worker), ein Codec-Plugin mit parallelen Aufrufen für D-Star, DMR, P25 und M17, alle Module transcodieren 30 s lang gleichzeitig, gespeist von einem Test-Client statt urfd; Latenz von einem Frame herein bis zu seinem Frame jedes Codecs hinaus:

  | Last | vorher: ein Thread je Codec | Modul-Worker |
  |---|---|---|
  | ein Stream allein, jeder Eingangs-Codec nacheinander | Median 0,2-0,7 ms, höchstens 5,7 ms | Median 0,2-0,7 ms, höchstens 1,2 ms |
  | 26 Module, alle sechs Eingangs-Codecs gemischt | 43 % eines Kerns, 99 % in 0,8 ms, höchstens 4,8 ms | 36 %, 0,8 ms, höchstens 3,7 ms |
  | dasselbe, alle 26 Frames im selben Moment | 38 %, 99 % in 8,0 ms, höchstens 9,9 ms | 37 %, 3,6 ms, höchstens 6,5 ms |
  | 26 Module USRP (am schwersten: jeder Codec wird codiert) | 60 %, 0,6 ms, höchstens 7,0 ms | 50 %, 0,5 ms, höchstens 3,3 ms |

  Kein Frame ging verloren. Ein Thread je Codec verhielt sich auf einem Kern wie auf vieren: Jeder Codec-Thread wartete auf das Plugin, das tcd immer nur aus einem Thread aufrief.
- Modul-Worker auf einem Raspberry Pi 4, dieselbe Last: mit 4 Workern (`auto`) kein Frame verloren, 26 Module gemischt 99 % in 2,3 ms (alle 26 Frames im selben Moment 9,4 ms), 26 × USRP 99 % in 2,0 ms bei etwa einem Drittel jedes Kerns. Ein Worker schafft 26 Module gemischt (zwei Drittel eines Kerns), aber nicht 26 × USRP: Der Kern ist voll, und der Worker verwirft Frames, die älter als 400 ms sind. Ein Pi 4 braucht für 26 belegte Module `Workers = auto` (oder 2 und mehr); auf einem Pi 5 reicht ein Worker.
