// MorseLink - files: the settings, and the JSON-lines logs.
//
// Logs go to Download/MorseLink on the phone when Android allows it there
// (easy to find, and to copy to a PC over USB); otherwise to the app's own
// folder, Android/data/com.morselink.morselink_app/files/logs (also
// reachable over USB from a PC). The Settings page shows which.
//
// The app's own folders come from MainActivity.kt over a small platform
// channel (instead of the path_provider plugin, whose current Android
// version needs the NDK and CMake to build).

import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';

/// MainActivity.kt answers "dirs" with {"files": <private folder>,
/// "external": <Android/data/<app>/files, or null>}.
const MethodChannel _paths = MethodChannel('morselink/paths');

/// The app's own folders (an empty map if the question fails).
Future<Map<String, String?>> _appDirs() async {
  try {
    return await _paths.invokeMapMethod<String, String?>('dirs') ?? <String, String?>{};
  } catch (_) {
    return <String, String?>{};
  }
}

/// Where the logs go (see the top of the file).
Future<Directory> logDirectory() async {
  final dl = Directory('/storage/emulated/0/Download/MorseLink');
  try {
    await dl.create(recursive: true);
    final probe = File('${dl.path}/probe.txt');
    await probe.writeAsString('ok');
    await probe.delete();
    return dl;
  } catch (_) {}
  final dirs = await _appDirs();
  final base = dirs['external'] ?? dirs['files'];
  if (base == null) throw const FileSystemException('no folder for the logs');
  final d = Directory('$base/logs');
  await d.create(recursive: true);
  return d;
}

/// One JSON-lines file: one object per line, each with "t" = UTC wall-clock
/// milliseconds (docs/TEST_PLAN.md section 4.4). Writes are synchronous and
/// small, so a crash loses at most what the OS hadn't written yet.
class JsonlLog {
  RandomAccessFile? _f;
  String path = '';
  int records = 0;

  bool get isOpen => _f != null;

  /// Open (or continue) `dir`/`name`. False if it can't be written.
  bool open(Directory dir, String name) {
    close();
    try {
      path = '${dir.path}/$name';
      _f = File(path).openSync(mode: FileMode.append);
      records = 0;
      return true;
    } catch (_) {
      _f = null;
      return false;
    }
  }

  /// Write one record (its "t" is added here unless it has one).
  void write(Map<String, Object?> rec) {
    final f = _f;
    if (f == null) return;
    final r = <String, Object?>{'t': DateTime.now().toUtc().millisecondsSinceEpoch};
    r.addAll(rec);
    try {
      f.writeStringSync('${jsonEncode(r)}\n');
      records++;
    } catch (_) {}
  }

  void flush() {
    try {
      _f?.flushSync();
    } catch (_) {}
  }

  void close() {
    final f = _f;
    _f = null;
    if (f == null) return;
    try {
      f.flushSync();
      f.closeSync();
    } catch (_) {}
  }
}

/// What the app remembers between runs (a small JSON file in its own folder).
class Settings {
  String deviceId = ''; // the board's Bluetooth address
  String deviceName = ''; // "MorseLink-K7Q"
  String prefix = 'morselink-za7q'; // MQTT topic prefix: the same on every phone of the project
  String broker = 'broker.hivemq.com';
  String phoneName = ''; // for the test logs ("Galaxy A54", "borrowed Pixel")
  String baseDesc = ''; // test set-up, remembered for the next session
  String baseHeight = '';
  String battery = '';
  String txDbm = '22'; // (the test form's power field, for boards before v0.6.0, which don't report it)
  bool background = true; // keep running with the screen off while a board is chosen (lib/background.dart)

  File? _file;

  static Future<Settings> load() async {
    final s = Settings();
    try {
      final dir = (await _appDirs())['files'];
      if (dir == null) return s; // (no folder: the settings last for this run only)
      s._file = File('$dir/settings.json');
      if (await s._file!.exists()) {
        final m = jsonDecode(await s._file!.readAsString());
        if (m is Map) {
          String pick(String k, String d) => m[k] is String ? m[k] as String : d;
          s.deviceId = pick('deviceId', s.deviceId);
          s.deviceName = pick('deviceName', s.deviceName);
          s.prefix = pick('prefix', s.prefix);
          s.broker = pick('broker', s.broker);
          s.phoneName = pick('phoneName', s.phoneName);
          s.baseDesc = pick('baseDesc', s.baseDesc);
          s.baseHeight = pick('baseHeight', s.baseHeight);
          s.battery = pick('battery', s.battery);
          s.txDbm = pick('txDbm', s.txDbm);
          if (m['background'] is bool) s.background = m['background'] as bool;
        }
      }
    } catch (_) {}
    return s;
  }

  Future<void> save() async {
    final f = _file;
    if (f == null) return;
    try {
      await f.writeAsString(jsonEncode({
        'deviceId': deviceId,
        'deviceName': deviceName,
        'prefix': prefix,
        'broker': broker,
        'phoneName': phoneName,
        'baseDesc': baseDesc,
        'baseHeight': baseHeight,
        'battery': battery,
        'txDbm': txDbm,
        'background': background,
      }));
    } catch (_) {}
  }
}
