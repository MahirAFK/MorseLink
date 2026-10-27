// MorseLink - settings: the internet relay, where the logs go, the board,
// and (v0.2.0) running in the background.

import 'package:flutter/material.dart';

import '../app_state.dart';

class SettingsPage extends StatefulWidget {
  final AppState app;

  const SettingsPage({super.key, required this.app});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final TextEditingController _prefix;
  late final TextEditingController _broker;
  late final TextEditingController _phone;

  AppState get app => widget.app;

  @override
  void initState() {
    super.initState();
    _prefix = TextEditingController(text: app.settings.prefix);
    _broker = TextEditingController(text: app.settings.broker);
    _phone = TextEditingController(text: app.settings.phoneName);
  }

  @override
  void dispose() {
    _prefix.dispose();
    _broker.dispose();
    _phone.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    app.settings.phoneName = _phone.text.trim();
    await app.applyRelaySettings(_prefix.text, _broker.text);
    if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Saved')));
  }

  Future<void> _forget() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Forget this board?'),
        content: const Text('The app stops connecting to it. To use it again: O on the device, then Find.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Forget')),
        ],
      ),
    );
    if (ok == true) await app.forgetDevice();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: app,
      builder: (context, _) => Scaffold(
        appBar: AppBar(title: const Text('Settings')),
        body: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            Text('Internet relay (MQTT)', style: Theme.of(context).textTheme.titleMedium),
            const Text('Both phones must use the same prefix and broker.'),
            TextField(controller: _prefix, decoration: const InputDecoration(labelText: 'Topic prefix')),
            TextField(controller: _broker, decoration: const InputDecoration(labelText: 'Broker (port 1883)')),
            TextField(controller: _phone, decoration: const InputDecoration(labelText: 'This phone (for the test logs)')),
            const SizedBox(height: 8),
            FilledButton(onPressed: _save, child: const Text('Save and reconnect')),
            const SizedBox(height: 8),
            Text('Relay: ${app.mqttState}, topic ${app.settings.prefix}/${app.ownId.isEmpty ? '<ID>' : app.ownId}'),
            const Divider(height: 32),
            Text('Logs', style: Theme.of(context).textTheme.titleMedium),
            Text(app.logDir?.path ?? '(no folder)'),
            const Text('One file per day (everything the board said), and one per test session. '
                'Copy them to the PC over USB, or with any file manager.'),
            const Divider(height: 32),
            Text('Board', style: Theme.of(context).textTheme.titleMedium),
            Text(app.settings.deviceId.isEmpty
                ? 'None chosen'
                : '${app.settings.deviceName} (${app.settings.deviceId}), ${app.bleState}, MTU ${app.mtu}'),
            Text('Firmware ${app.fw.isEmpty ? '?' : app.fw} (protocol ${app.protocol == 0 ? '?' : app.protocol}), '
                'app $appVersion'),
            if (app.radio != null) Text('Radio: ${app.radio!.text}'),
            if (app.bleWhy.isNotEmpty) Text('Bluetooth: ${app.bleWhy}'),
            const SizedBox(height: 8),
            if (app.settings.deviceId.isNotEmpty)
              OutlinedButton(onPressed: _forget, child: const Text('Forget this board')),
            const Divider(height: 32),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Keep running in the background'),
              subtitle: const Text('While a board is chosen, a small permanent notification keeps the app going '
                  'with the screen off, so Bluetooth and the internet relay don\'t stop when the phone sleeps.'),
              value: app.settings.background,
              onChanged: app.setBackground,
            ),
            const SizedBox(height: 8),
            const Text('Also set Settings > Apps > MorseLink > Battery > Unrestricted (Samsung: and keep MorseLink out '
                'of "Sleeping apps"). The screen stays on while the app is open; during a test session the GPS '
                'notification keeps it running too.'),
          ],
        ),
      ),
    );
  }
}
