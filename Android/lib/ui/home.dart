import 'package:flutter/material.dart';

import '../app_state.dart';
import '../model.dart';
import 'canvas.dart';
import 'editor.dart';
import 'send_dialog.dart';
import 'theme.dart';
import 'widgets.dart';

class HomeScreen extends StatefulWidget {
  final AppState app;
  const HomeScreen(this.app, {super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  int _tab = 0;
  AppState get app => widget.app;

  void _open(Design d) => Navigator.of(context).push(MaterialPageRoute(builder: (_) => EditorScreen(app, d.id)));

  Future<void> _create() async {
    final d = await showDialog<Design>(context: context, builder: (_) => NewDesignDialog(app));
    if (d != null && mounted) _open(d);
  }

  @override
  Widget build(BuildContext context) {
    final wide = isWide(context);
    return ListenableBuilder(
      listenable: app,
      builder: (context, _) {
        final body = _tab == 0 ? _designs(context) : _machines(context);
        return Scaffold(
          appBar: AppBar(
            title: const Text('PhobiCCCP', style: TextStyle(fontWeight: FontWeight.bold)),
            actions: [IconButton(onPressed: app.syncNow, icon: const Icon(Icons.sync), tooltip: 'Sync now')],
            bottom: app.syncing
                ? const PreferredSize(preferredSize: Size.fromHeight(2), child: LinearProgressIndicator(minHeight: 2))
                : null,
          ),
          body: wide
              ? Row(children: [
                  NavigationRail(
                    backgroundColor: Brand.surface,
                    selectedIndex: _tab,
                    onDestinationSelected: (i) => setState(() => _tab = i),
                    labelType: NavigationRailLabelType.all,
                    leading: Padding(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      child: FloatingActionButton(
                          onPressed: _create, backgroundColor: Brand.red, tooltip: 'New design', child: const Icon(Icons.add)),
                    ),
                    destinations: const [
                      NavigationRailDestination(icon: Icon(Icons.dashboard_outlined), label: Text('Designs')),
                      NavigationRailDestination(icon: Icon(Icons.precision_manufacturing_outlined), label: Text('Machines')),
                    ],
                  ),
                  Expanded(child: body),
                ])
              : body,
          bottomNavigationBar: wide
              ? null
              : NavigationBar(
                  selectedIndex: _tab,
                  onDestinationSelected: (i) => setState(() => _tab = i),
                  destinations: const [
                    NavigationDestination(icon: Icon(Icons.dashboard_outlined), label: 'Designs'),
                    NavigationDestination(icon: Icon(Icons.precision_manufacturing_outlined), label: 'Machines'),
                  ],
                ),
          floatingActionButton: !wide && _tab == 0
              ? FloatingActionButton(onPressed: _create, backgroundColor: Brand.red, child: const Icon(Icons.add))
              : null,
        );
      },
    );
  }

  Widget _designs(BuildContext context) {
    if (app.designs.isEmpty) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(32),
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            Text('No designs yet.', style: TextStyle(fontSize: 20)),
            SizedBox(height: 8),
            Text(
                'Make one with +. Designs on this pad are kept in step with every PhobiCCCP on the same network, '
                'so one made on another pad shows up here too.',
                textAlign: TextAlign.center,
                style: TextStyle(color: Brand.textDim)),
          ]),
        ),
      );
    }
    return GridView.builder(
      padding: const EdgeInsets.all(16),
      gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
          maxCrossAxisExtent: 360, mainAxisSpacing: 16, crossAxisSpacing: 16, childAspectRatio: 1.05),
      itemCount: app.designs.length,
      itemBuilder: (context, i) {
        final d = app.designs[i];
        return Card(
          clipBehavior: Clip.antiAlias,
          child: InkWell(
            onTap: () => _open(d),
            child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              Expanded(child: Container(color: Brand.background, child: DesignThumbnail(d))),
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 4, 8),
                child: Row(children: [
                  Expanded(
                    child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                      Text(d.name.isEmpty ? 'Untitled' : d.name,
                          style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600), overflow: TextOverflow.ellipsis),
                      Text(
                          '${fmt(d.stock.width)} × ${fmt(d.stock.height)} × ${fmt(d.stock.thickness)} mm · '
                          '${d.toolpaths.length} toolpaths',
                          style: const TextStyle(color: Brand.textDim, fontSize: 12)),
                    ]),
                  ),
                  IconButton(
                    icon: const Icon(Icons.send, color: Brand.gold),
                    tooltip: 'Send',
                    onPressed: () => showSendDialog(context, app, d),
                  ),
                  PopupMenuButton<String>(
                    onSelected: (v) => v == 'dup' ? app.duplicateDesign(d.id) : _confirmDelete(d),
                    itemBuilder: (_) => const [
                      PopupMenuItem(value: 'dup', child: Text('Duplicate')),
                      PopupMenuItem(value: 'del', child: Text('Delete')),
                    ],
                  ),
                ]),
              ),
            ]),
          ),
        );
      },
    );
  }

  Future<void> _confirmDelete(Design d) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: Text('Delete “${d.name}”?'),
        content: const Text('It is removed from this pad and from every PhobiCCCP it syncs with.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(c, true), child: const Text('Delete')),
        ],
      ),
    );
    if (ok == true) app.deleteDesign(d.id);
  }

  Widget _machines(BuildContext context) {
    final now = app.now;
    return ListView(padding: const EdgeInsets.all(16), children: [
      if (app.computers.isEmpty) ...const [
        Text('Looking for PhobiCCCP on this network…', style: TextStyle(fontSize: 18)),
        SizedBox(height: 6),
        Text(
            'Start phobiCCCP on a computer on the same Wi-Fi, with File › Receive from Syntexophobic and pads turned on. '
            'Over Tailscale or across networks, add it by name or address below.',
            style: TextStyle(color: Brand.textDim)),
        SizedBox(height: 16),
      ],
      Wrap(spacing: 16, runSpacing: 16, children: [
        for (final c in app.computers) SizedBox(width: 420, child: _computerCard(context, c, now)),
      ]),
      const SizedBox(height: 16),
      Align(
        alignment: Alignment.centerLeft,
        child: OutlinedButton.icon(
            onPressed: _addHost, icon: const Icon(Icons.add_link), label: const Text('Add a computer by name or address')),
      ),
    ]);
  }

  Widget _computerCard(BuildContext context, Computer c, int now) {
    final online = c.reachable(now) || (c.error == null && c.desktop.online(now));
    final info = c.info;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Icon(Icons.circle, size: 12, color: online ? Brand.good : Brand.textDim),
            const SizedBox(width: 8),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(c.name, style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
                Text(
                    [if (c.desktop.address != c.name) c.desktop.address, if (info != null && info.version.isNotEmpty) 'phobiCCCP ${info.version}']
                        .join(' · '),
                    style: const TextStyle(color: Brand.textDim, fontSize: 12)),
              ]),
            ),
            if (c.desktop.manual)
              IconButton(onPressed: () => app.removeHost(c.desktop.typed), icon: const Icon(Icons.delete_outline), tooltip: 'Forget'),
          ]),
          if (c.error != null) Text(c.error!, style: TextStyle(color: Theme.of(context).colorScheme.error, fontSize: 12)),
          if (info != null) ...[
            const SizedBox(height: 8),
            Text(info.status.words() + (info.document.isNotEmpty ? ' · open: ${info.document}' : ''),
                style: TextStyle(color: info.status.readyToCut ? Brand.good : Brand.amber)),
            const SizedBox(height: 8),
            const Text('Machines', style: TextStyle(color: Brand.textDim, fontSize: 12)),
            RadioGroup<String>(
              groupValue: info.activeMachine,
              onChanged: (id) async {
                if (id == null || !online) return;
                final m = info.machines.firstWhere((m) => m.id == id);
                try {
                  await app.selectMachine(c, id);
                  if (context.mounted) toast(context, '${c.name} now uses ${m.name}');
                } catch (e) {
                  if (context.mounted) toast(context, 'Could not switch: $e');
                }
              },
              child: Column(children: [
                for (final m in info.machines)
                  RadioListTile<String>(
                    value: m.id,
                    enabled: online,
                    contentPadding: EdgeInsets.zero,
                    dense: true,
                    title: Text(m.name),
                    subtitle: m.size.isEmpty ? null : Text(m.size),
                  ),
              ]),
            ),
            Text(
                info.allowStart
                    ? 'Pads may start cutting here.'
                    : 'Starting from a pad is off here (Machine › Pads may start cutting).',
                style: const TextStyle(color: Brand.textDim, fontSize: 12)),
          ],
        ]),
      ),
    );
  }

  Future<void> _addHost() async {
    final c = TextEditingController();
    final host = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Add a computer'),
        content: TextField(
            controller: c,
            autofocus: true,
            decoration: const InputDecoration(
                labelText: 'Name or address', helperText: 'For example a Tailscale name, or 192.168.1.20')),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, c.text), child: const Text('Add')),
        ],
      ),
    );
    if (host != null) app.addHost(host);
  }
}

class NewDesignDialog extends StatefulWidget {
  final AppState app;
  const NewDesignDialog(this.app, {super.key});
  @override
  State<NewDesignDialog> createState() => _NewDesignDialogState();
}

class _NewDesignDialogState extends State<NewDesignDialog> {
  final _name = TextEditingController();
  Stock _stock = const Stock();
  String _material = 'softwood';

  @override
  Widget build(BuildContext context) => AlertDialog(
        title: const Text('New design'),
        content: SizedBox(
          width: 520,
          child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            TextField(controller: _name, autofocus: true, decoration: const InputDecoration(labelText: 'Name', border: OutlineInputBorder())),
            const SizedBox(height: 16),
            StockFields(
                stock: _stock,
                material: _material,
                tools: widget.app.tools,
                onStock: (s) => setState(() => _stock = s),
                onMaterial: (m) => setState(() => _material = m)),
          ]),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, widget.app.newDesign(_name.text, _stock, _material)),
              child: const Text('Create')),
        ],
      );
}
