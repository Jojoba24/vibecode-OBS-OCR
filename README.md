# obs-cue-chapters

Lizenz: GPL-2.0-or-later (wie libobs).

OBS-Videofilter für NDI-Quellen (DistroAV): liest per OCR (Tesseract) die Cue-Anzeige `CUE000.000`
aus dem Video und setzt bei jedem Cue-Wechsel einen **benannten Kapitelmarker** in die laufende Aufnahme.

## Voraussetzungen
- OBS Studio **≥ 30.2**, Aufnahmeformat **Hybrid MP4** (Einstellungen → Ausgabe → Aufnahme; Erweiterter Modus).
  Normales/fragmentiertes MP4 unterstützt keine Kapitelmarker.
- Native OBS-Installation (deb/PPA). Flatpak/Snap: Tesseract ist in der Sandbox nicht verfügbar.

## Bauen (Ubuntu/Debian)
    sudo apt install build-essential cmake pkg-config libobs-dev libtesseract-dev libleptonica-dev tesseract-ocr-eng
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
    cmake --install build     # nach ~/.config/obs-studio/plugins/obs-cue-chapters/bin/64bit/

## Benutzung
1. Rechtsklick auf die NDI-Quelle → Filter → **Effekt-Filter/Audio-Video-Filter** (Async-Filter) → *Cue OCR -> Chapter Markers*.
2. Die gestrichelte Linie zeigt den ausgelesenen Bereich (liegt knapp *außerhalb*). Mit Left/Top/Width/Height auf den Cue-Text setzen.
   Danach „Show ROI frame“ **ausschalten** (sonst ist der Rahmen in der Aufnahme).
3. „Write preprocessed image to file“ aktivieren und `/tmp/obs-cue-roi.pgm` in einem Bildbetrachter öffnen:
   Ziel ist **schwarzer, zusammenhängender Text auf weißem Grund** ohne Rauschpunkte.
4. „Refresh status“ zeigt Rohtext, Konfidenz, Treffer und aktiven Cue. „Write test marker“ prüft die Marker-Kette.

## Kalibrierung bei Rauschen / Pixeltext
| Problem | Einstellung |
|---|---|
| Rauschpunkte im Bild | Temporal averaging erhöhen (4 → 8). Weißer, statischer Text bleibt, Rauschen mittelt sich weg |
| Text zerfällt / Lücken | Smoothing 2–4, Stroke width + |
| Zeichen kleben zusammen | Stroke width − , Smoothing runter |
| Hintergrund wird mitgelesen | Threshold: Manual, Wert hoch (Videobereich-Weiß ≈ 235, z. B. 170–200) |
| Führende Null fehlt (`CUE12.003`) | Tesseract verschluckt gern die erste Null nach dem „E“. Standardmäßig wird der Teil vor dem Punkt auf 3 Stellen mit Nullen aufgefüllt („Zero-pad“) |
| Falsche/flackernde Cues | „Identical reads required“ erhöhen; Regex strenger |
| Marker kommt zu spät | Averaging, Pause, Identical reads senken (Latenz ≈ Frames/fps + Reads × Pause) |

Format anpassen: *Prefix* (`CUE`) + *Regex* für den Rest (`[0-9]{3}\.[0-9]{3}`).
Tesseracts Konfidenzwert ist bei winzigen Pixelschriften unzuverlässig, daher ist „Minimum OCR confidence“ standardmäßig 0;
gefiltert wird über Regex + Mehrfachbestätigung.

## Grenzen
- Der Marker wird zum Zeitpunkt der Erkennung gesetzt (die API erlaubt kein Rückdatieren), also leicht nach dem tatsächlichen Cue-Wechsel.
- Unterstützte Frameformate: I420, NV12, I422, I444, UYVY, YUY2, YVYU, BGRA/BGRX/RGBA, Y800, I010, P010 (NDI liefert i. d. R. UYVY oder BGRA).

## Test
`tools/smoke_test.sh build/obs-cue-chapters.so` startet ein echtes OBS headless (Xvfb) mit einem synthetischen,
verrauschten Video mit wechselnden Cues und gibt die erkannten Cues aus. Getestet mit OBS 30.0.2 (Ubuntu 24.04):
alle 6 Test-Cues wurden in der richtigen Reihenfolge erkannt. Die Kapitelmarker-API fehlt dort erwartungsgemäß
(< 30.2), der Aufruf-Pfad wird aber durchlaufen.

Für den echten Test mit Marker in der Datei: Ubuntu-24.04-VM/Desktop, OBS aus dem PPA (`sudo add-apt-repository ppa:obsproject/obs-studio`),
DistroAV aus den GitHub-Releases, Aufnahmeformat **Hybrid MP4**, danach die Datei mit `ffprobe -show_chapters aufnahme.mp4` prüfen.
