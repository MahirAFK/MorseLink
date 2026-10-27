// MorseLink phone app: shows what the device is doing, relays its packets
// over the internet, and runs the field tests (docs/TEST_PLAN.md).
//
// Files: app_state.dart (everything the app knows and does), ble_link.dart
// (Bluetooth), relay.dart (MQTT), protocol.dart (the device's lines),
// storage.dart (settings and logs), ui/ (the screens).

import 'package:flutter/material.dart';

import 'app_state.dart';
import 'storage.dart';
import 'ui/home_page.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final settings = await Settings.load();
  final app = AppState(settings);
  runApp(MorseLinkApp(app: app));
  app.init();
}

class MorseLinkApp extends StatelessWidget {
  final AppState app;

  const MorseLinkApp({super.key, required this.app});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'MorseLink',
      theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
      darkTheme: ThemeData(colorSchemeSeed: Colors.indigo, brightness: Brightness.dark, useMaterial3: true),
      home: HomePage(app: app),
    );
  }
}
