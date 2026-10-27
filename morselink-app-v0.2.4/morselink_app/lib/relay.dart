// MorseLink - the internet relay (spec section 12): MQTT on a public broker.
//
// Topics (the prefix is the same on every phone of the project):
//   <prefix>/<ID>        packets for device <ID>, base64 (the device's RL lines)
//   <prefix>/<ID>/ctl    control for <ID>'s phone, JSON: test start / stop,
//                        and the base phone's status and position for the rover
// Each phone subscribes to its own device's two topics.

import 'dart:async';
import 'dart:math';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

class Relay {
  /// A message arrived on one of our topics.
  final void Function(String topic, String payload) onMessage;

  /// "off", "connecting", "connected", "reconnecting", "disconnected", "no internet".
  final void Function(String state) onState;

  Relay({required this.onMessage, required this.onState});

  String host = 'broker.hivemq.com';
  int port = 1883;
  String prefix = 'morselink-za7q';
  String state = 'off';

  MqttServerClient? _c;
  String _own = '';
  Timer? _retry;
  StreamSubscription? _sub;

  bool get connected => _c?.connectionStatus?.state == MqttConnectionState.connected;

  /// Connect for device `ownId` (again, if the ID or the settings changed).
  Future<void> start(String ownId) async {
    if (ownId.isEmpty) return;
    if (ownId == _own && _c != null) return;
    await stop();
    _own = ownId;
    _connect();
  }

  Future<void> stop() async {
    _retry?.cancel();
    await _sub?.cancel();
    _sub = null;
    final c = _c;
    _c = null;
    if (c != null) {
      c.autoReconnect = false;
      try {
        c.disconnect();
      } catch (_) {}
    }
    _set('off');
  }

  void _set(String s) {
    state = s;
    onState(s);
  }

  void _later() {
    _retry?.cancel();
    _retry = Timer(const Duration(seconds: 10), _connect);
  }

  /// Connect, subscribe, listen. The first connection is retried here every
  /// 10 s; once connected, the client reconnects (and resubscribes) by itself.
  Future<void> _connect() async {
    final own = _own;
    if (own.isEmpty) return;
    final c = MqttServerClient.withPort(host, 'ml-$own-${Random().nextInt(1 << 30)}', port);
    c.setProtocolV311();
    c.keepAlivePeriod = 30;
    c.autoReconnect = true;
    c.logging(on: false);
    c.onConnected = () => _set('connected');
    c.onDisconnected = () {
      if (_c == c) _set('disconnected');
    };
    c.onAutoReconnect = () => _set('reconnecting');
    c.onAutoReconnected = () => _set('connected');
    _c = c;
    _set('connecting');
    try {
      await c.connect();
    } catch (_) {
      // no network, or the broker didn't answer
    }
    if (_c != c) return; // stopped meanwhile
    if (c.connectionStatus?.state != MqttConnectionState.connected) {
      _c = null;
      try {
        c.disconnect();
      } catch (_) {}
      _set('no internet');
      _later();
      return;
    }
    c.subscribe('$prefix/$own', MqttQos.atLeastOnce);
    c.subscribe('$prefix/$own/ctl', MqttQos.atLeastOnce);
    _sub = c.updates?.listen((msgs) {
      for (final m in msgs) {
        final pm = m.payload as MqttPublishMessage;
        final text = MqttPublishPayload.bytesToStringAsString(pm.payload.message);
        onMessage('${m.topic}', text);
      }
    });
    _set('connected');
  }

  /// Publish text to a topic. False if not connected (then it is not sent).
  bool publish(String topic, String text) {
    final c = _c;
    if (c == null || c.connectionStatus?.state != MqttConnectionState.connected) return false;
    final b = MqttClientPayloadBuilder();
    b.addString(text);
    c.publishMessage(topic, MqttQos.atLeastOnce, b.payload!);
    return true;
  }
}
