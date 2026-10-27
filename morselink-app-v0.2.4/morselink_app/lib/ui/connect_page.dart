// MorseLink - finding the device: a Bluetooth scan for "MorseLink-..." names.

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../app_state.dart';
import '../ble_link.dart';

class ConnectPage extends StatefulWidget {
  final AppState app;

  const ConnectPage({super.key, required this.app});

  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  StreamSubscription<List<ScanResult>>? _results;
  StreamSubscription<bool>? _scanning;
  List<ScanResult> _found = [];
  bool _isScanning = false;
  String _error = '';

  static String _name(ScanResult r) =>
      r.advertisementData.advName.isNotEmpty ? r.advertisementData.advName : r.device.platformName;

  @override
  void initState() {
    super.initState();
    _results = FlutterBluePlus.scanResults.listen((list) {
      if (!mounted) return;
      setState(() => _found = list.where((r) => _name(r).startsWith('MorseLink')).toList());
    });
    _scanning = FlutterBluePlus.isScanning.listen((s) {
      if (mounted) setState(() => _isScanning = s);
    });
    WidgetsBinding.instance.addPostFrameCallback((_) => _scan()); // (after the first frame: _scan calls setState)
  }

  Future<void> _scan() async {
    setState(() => _error = '');
    try {
      if (await adapterState() != BluetoothAdapterState.on) {
        try {
          await FlutterBluePlus.turnOn();
        } catch (_) {}
      }
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 12));
    } catch (e) {
      if (mounted) setState(() => _error = 'Scan failed: $e');
    }
  }

  Future<void> _choose(ScanResult r) async {
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    await widget.app.useDevice(r.device, _name(r));
    if (mounted) Navigator.pop(context);
  }

  @override
  void dispose() {
    _results?.cancel();
    _scanning?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Find your MorseLink'),
        actions: [
          IconButton(icon: const Icon(Icons.refresh), onPressed: _isScanning ? null : _scan),
        ],
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Padding(
            padding: EdgeInsets.all(16),
            child: Text(
              'First time with this phone: on the device, command mode, O, hold 2 s. '
              'You then have 60 s: tap the device below, and tap Pair when Android asks. '
              'The device plays OK.\n\nAlready paired: just tap it.',
            ),
          ),
          if (_isScanning) const LinearProgressIndicator(),
          if (_error.isNotEmpty) Padding(padding: const EdgeInsets.all(16), child: Text(_error)),
          Expanded(
            child: _found.isEmpty
                ? Center(child: Text(_isScanning ? 'Looking...' : 'None found. Is the device on? Tap refresh.'))
                : ListView(
                    children: [
                      for (final r in _found)
                        ListTile(
                          leading: const Icon(Icons.watch),
                          title: Text(_name(r)),
                          subtitle: Text('${r.device.remoteId.str}   signal ${r.rssi} dBm'),
                          onTap: () => _choose(r),
                        ),
                    ],
                  ),
          ),
        ],
      ),
    );
  }
}
