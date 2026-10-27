// MorseLink - keeping the app running with the screen off (v0.2.0).
//
// Android pauses, and on newer versions freezes, an app it no longer shows,
// within seconds: the Bluetooth link to the board and the internet relay
// then stop until the phone is woken. A foreground service (MainActivity.kt,
// KeepAliveService) prevents that; Android requires it to show a small
// permanent notification, which says how the board is. It runs while a
// board is chosen and the Settings switch is on, and stops when the app is
// closed (swiped away) or the board forgotten.
//
// Every call is safe to repeat and never throws: if Android refuses (no
// "Nearby devices" permission yet, or the app is in the background when it
// first starts), the next call tries again.

import 'package:flutter/services.dart';

class Background {
  static const MethodChannel _ch = MethodChannel('morselink/service');
  static String _shown = ''; // the text the notification has ("" = not running, as far as we know)

  /// Run the service with this notification text (start it, or update the
  /// text). True if it runs. Unchanged text isn't sent again unless `force`
  /// (the app does that once a minute, in case Android stopped it).
  static Future<bool> show(String text, {bool force = false}) async {
    if (text == _shown && !force) return true;
    try {
      final ok = await _ch.invokeMethod<bool>('show', {'text': text}) ?? false;
      _shown = ok ? text : '';
      return ok;
    } catch (_) {
      _shown = '';
      return false;
    }
  }

  /// Stop the service (the notification goes).
  static Future<void> stop() async {
    _shown = '';
    try {
      await _ch.invokeMethod<bool>('stop');
    } catch (_) {}
  }
}
