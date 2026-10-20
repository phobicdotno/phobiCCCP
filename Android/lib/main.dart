// PhobiCCCP for pads: design on an Android tablet or iPad, synced with every
// phobiCCCP on the network, sent to any of them and any of their machines.

import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import 'app_state.dart';
import 'store.dart';
import 'ui/main_window.dart';
import 'ui/theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final dir = await getApplicationSupportDirectory();
  // Not backed up and not carried over to another device: Android's
  // no_backup (beside getFilesDir, which [dir] is there), iOS's Caches.
  final device = Platform.isAndroid ? Directory('${dir.parent.path}/no_backup') : await getApplicationCacheDirectory();
  final store = DesignStore(Directory('${dir.path}/designs'), device: device);
  final app = AppState(store, Settings(File('${dir.path}/settings.json')))..start();
  runApp(PhobiApp(app));
}

class PhobiApp extends StatelessWidget {
  final AppState app;
  const PhobiApp(this.app, {super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'PhobiCCCP',
        debugShowCheckedModeBanner: false,
        theme: phobiTheme(),
        home: MainWindow(app),
      );
}
