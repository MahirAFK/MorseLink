# MorseLink

A pair of haptic wearables that let two people exchange text in Morse code by touch. You key Morse on one button and the device decodes it. The text travels to the partner's device by LoRa, with an optional phone relay as a second path, and is felt as vibrations on two motors on the forearm. An Android companion app shows the Morse as it is keyed and felt, and relays messages over the internet when the radio can't reach.

Final-year project, BSc Mechatronics, University of Cape Town (EEE4022S, 2026).
Author: Mahir Khan. Supervisor: Jarryd Son.

## Repository contents

| Folder | What it holds |
|---|---|
| `morselink-v0.6.4/morselink` | Device firmware (C, ESP-IDF v6.1): source, host tests, emulator test script, specification, test plan and user manual |
| `morselink-app-v0.2.4/morselink_app` | Companion app (Dart/Flutter, Android) |
| `morselink-enclosure-v10` | Enclosure model, version 10 (OpenSCAD source and STL files) |
| `Scheme` | Circuit schematic (KiCad) |
| `Results` | Field data from every test: both phones' session files, battery run-down logs, current-meter readings, motor recordings and the anonymous questionnaire answers |

## Hardware

- LilyGO T-Beam V1.2 (ESP32, SX1262 LoRa radio at 869.525 MHz, AXP2101 power management)
- Samsung INR18650-30Q cell
- Two Pololu #1638 ERM vibration motors, each driven through a 2N2222 transistor with a 330 Ω base resistor, PWM at 20 kHz
- One key on GPIO14 (internal pull-up)

The full schematic and bill of materials are in the report (Appendix D).

## Building

**Firmware.** Install ESP-IDF v6.1, then from `morselink-v0.6.4/morselink`:

```
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

The device commands are listed in the user manual in the same folder.

**App.** With Flutter installed, from `morselink-app-v0.2.4/morselink_app`:

```
flutter pub get
flutter run
```

**Enclosure.** Open the `.scad` file in OpenSCAD, or print the STL files directly. Version 10 was printed on a Bambu Lab H2D.

## Reproducing the report's figures

`make_figures.py` reads every test folder in `Results` and writes each figure and table of the report's results chapter. It also writes `numbers.txt`, which lists every number quoted in the text and the file it came from. It needs Python 3 with matplotlib:

```
pip install matplotlib
python make_figures.py
```

Run it from inside `Results`, with `analyse_tests.py` in the same folder. It writes `figures/`, `tables/` and `numbers.txt` next to itself.

## Report

The full report, *Development of Haptic Wearable Devices for Morse Code Communication*, describes the design, the tests and the results.
