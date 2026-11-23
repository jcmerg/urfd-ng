# Pegel im Detail

[English](LEVELS.md) | **Deutsch**

Die Einzelheiten hinter der AGC und dem Gain bei gleicher Betriebsart aus [tcd/README.de.md](../README.de.md#verstärkung).

## Wie die AGC arbeitet

Die AGC arbeitet je Stream (je Durchgang):
- ein 75-Hz-Hochpass auf dem Audio, so treiben Gleichanteil und Brummen weder den Pegel noch erreichen sie den Encoder
- der Sprachpegel folgt der oberen Hüllkurve der Sprache, nah an den Vokalen; Grundrauschen, Gate und Spracherkennung sehen nur das Band über 300 Hz, ein CTCSS-Ton oder Brummen zählt also nicht
- Anstiegen folgt sie schnell; der Pegel fällt (der Gain steigt) nur auf deutlich stimmhaften Frames klar über dem Grundrauschen, so pumpt der Gain nicht bei Atem, Zischen und Pausen
- die ersten stimmhaften Frames eines Streams setzen seinen Pegel schnell, damit die ersten Wörter eines leisen Sprechers nicht zu leise sind
- ein Grundrauschen je Stream: schwache, verrauschte Signale werden nicht in ihr Rauschen hinein angehoben, saubere Quellen bekommen den vollen Gain
- ein Rausch-Gate, das die Lücken zwischen Wörtern überbrückt (240 ms) und den Gain dann auf den Gate-Boden blendet, so bleiben Wortenden erhalten und Rauschen wird nicht verstärkt
- der Gain gleitet Sample für Sample über jeden Frame, und ein Spitzenbegrenzer knapp unter Vollaussteuerung verhindert Übersteuern

Die Verstärkungsgrenzen sind absichtlich ungleich: Absenken ist sicher, Anheben hebt auch Rauschen an. Typischer DMR-Eingang liegt um -35 dBFS, also braucht es etwa +20 dB, um ein Ziel von -16 dBFS zu erreichen.

## Analoge Quellen und Rauschunterdrückung

**Analoge Quellen (SVX, USRP)** liefern FM-Audio, dessen Pausen nicht still sind, sondern das Rauschen des Empfängers tragen, oft -45 bis -35 dBFS. Mit den Einstellungen für digitale Quellen würde die AGC dieses Rauschen zwischen den Wörtern um bis zu 30 dB anheben. AGC analog hat ein eigenes Gate, eine eigene Verstärkungsgrenze und einen eigenen Gate-Boden, so bleibt die Sprache deutlich über dem Rauschen (bei aufgezeichnetem FM-Funknetz-Verkehr 32 dB Abstand statt 19, siehe [Messungen](MEASUREMENTS.de.md#agc-bei-analogen-quellen)). Das Gate arbeitet auf dem Pegel nach dem `RxGain` von urfd, den also nahe 0 lassen und den Pegel der AGC überlassen. Ein Squelch-Schwanz ist lauter als jedes Gate: `SQL_TAIL_ELIM` im Empfänger des SvxLink-Knotens entfernt ihn an der Quelle.

**Rauschunterdrückung** (`AGCAnalogNoiseSuppress`): Das Gate wirkt nur zwischen den Wörtern; das FM-Rauschen unter der Sprache bleibt, und die Vocoder, vor allem AMBE, machen daraus einen blubbernden, roboterhaften Klang. Darum geht SVX- und USRP-Eingang zuerst durch den speexdsp-Präprozessor, ohne zusätzliche Verzögerung, mit einer Rauschschätzung je Modul. Dessen eigene AGC, VAD und Dereverb sind aus. -10 dB lassen ein leises, natürliches Grundrauschen und ganze Silben; stärkere Werte entfernen mehr Rauschen, schneiden aber Silbenanfänge ab und hinterlassen „musikalische“ Artefakte. Live im Monitor, 0 schaltet sie ab. Die eigene Rauschunterdrückung des AMBE-Chips schaltet tcd ab: Sie würde auf alles wirken, was der Kanal codiert, auch auf sauberes digitales Audio.

## Gleiche Betriebsart: der Gain im Frame

- **DMR (AMBE2+)**: Der Decoder rechnet γ = Δγ[b2] + 0,5·γ<sub>vorher</sub>, und die mittlere log2-Amplitude eines Frames ist γ, eine Einheit γ sind also 6 dB. Die Δγ-Tabelle ist für kleine b2 grob und für große fein. tcd verfolgt γ für den ursprünglichen und den veränderten Stream und wählt Frame für Frame das b2, das dem gewünschten Gain am nächsten kommt; der Rundungsfehler wird im nächsten Frame ausgeglichen. So hängt der Gain nicht vom Pegel ab: Gains von -12 bis +6 dB kommen bei Eingang von -40 bis -16 dBFS auf 0,8 dB genau an. Sehr leise Quellen lassen sich nicht über das hinaus anheben, was b2 = 31 erlaubt.
- **D-Star (AMBE)**: b2 hat 6 Bit, und der Decoder rechnet γ = 2·Δγ[b2] − 0,09 ohne den Frame davor; jeder Frame bekommt also das b2, das seinem eigenen γ plus dem gewünschten Gain am nächsten kommt. Die Stufen sind 0,7 bis 1,8 dB, über den ganzen Bereich gleich: -12 bis +3 dB kommen auf 0,15 dB genau an (0,2 bis 0,5 dB von Frame zu Frame), +6 dB und mehr bleiben bei lauter Sprache darunter, wo b2 an sein oberes Ende kommt.
- **P25 (IMBE)**: b2 setzt G1, den Mittelwert der log2-Amplituden; die Vorhersage des Decoders ist mittelwertfrei, der Pegel folgt b2 also ohne Gedächtnis. 64 Stufen von 1 bis 1,5 dB; -12 bis +6 dB kommen auf 0,2 dB genau an.
- **M17 (Codec2 3200 und 1600)**: die 5-Bit-Energie des Frames, 1,56 dB je Stufe (1600: beide Energien des 40-ms-Frames; die Datenhälfte bleibt). -12 dB kommen als höchstens -12,5 an, wie die Stufen fallen.

P25 und M17 sind mit jedem Decoder gleich: gemessen mit OP25 und mbelib-neo für IMBE, dem Codec2 von tcd für M17, gegen Frames beider Encoder.
