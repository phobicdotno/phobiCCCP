// The Toolpaths panel's Preview: the design's toolpaths as the computer
// exports them, drawn from above there, full screen with pinch zoom, with the
// time the job should take and each toolpath's share.

import 'package:flutter/material.dart';

import '../net.dart';
import 'theme.dart';

/// "12 s", "4 min 05 s", "1 h 12 min".
String previewTime(int sec) {
  if (sec < 60) return '$sec s';
  final h = sec ~/ 3600, m = (sec % 3600) ~/ 60, s = sec % 60;
  if (h > 0) return '$h h ${m.toString().padLeft(2, '0')} min';
  return '$m min ${s.toString().padLeft(2, '0')} s';
}

Future<void> showPreview(BuildContext context, String design, String computer, PreviewResult r) =>
    Navigator.of(context).push(MaterialPageRoute<void>(
      fullscreenDialog: true,
      builder: (_) => PreviewPage(design: design, computer: computer, result: r),
    ));

class PreviewPage extends StatelessWidget {
  final String design, computer;
  final PreviewResult result;
  const PreviewPage({super.key, required this.design, required this.computer, required this.result});

  @override
  Widget build(BuildContext context) {
    final r = result;
    final side = Container(
      color: C.panelHead,
      padding: const EdgeInsets.all(14),
      child: ListView(children: [
        const Text('TOTAL', style: TextStyle(fontSize: 12, color: C.label, letterSpacing: 1.2)),
        Text(previewTime(r.seconds), style: const TextStyle(fontSize: 26, fontWeight: FontWeight.w600, color: C.amber)),
        Text('as planned on $computer', style: const TextStyle(fontSize: 12, color: C.sub)),
        const SizedBox(height: 14),
        for (final (name, sec) in r.perToolpath)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 5),
            child: Row(children: [
              Expanded(child: Text(name, style: const TextStyle(fontSize: 15))),
              Text(previewTime(sec), style: const TextStyle(fontSize: 15, color: C.label)),
            ]),
          ),
        if (r.notes.isNotEmpty) ...[
          const SizedBox(height: 14),
          const Text('NOTES', style: TextStyle(fontSize: 12, color: C.label, letterSpacing: 1.2)),
          for (final n in r.notes)
            Padding(
              padding: const EdgeInsets.only(top: 6),
              child: Text(n, style: const TextStyle(fontSize: 13, color: Color(0xFFE0A040))),
            ),
        ],
      ]),
    );
    final picture = InteractiveViewer(
      minScale: 0.5,
      maxScale: 12,
      child: Center(child: Image.memory(r.png, gaplessPlayback: true, filterQuality: FilterQuality.medium)),
    );
    return Scaffold(
      backgroundColor: C.viewBg,
      appBar: AppBar(
        backgroundColor: C.panelHead,
        foregroundColor: C.text,
        title: Text('Preview of “$design”', style: const TextStyle(fontSize: 17)),
        toolbarHeight: 52,
      ),
      body: LayoutBuilder(
        builder: (context, box) => box.maxWidth > 700
            ? Row(children: [Expanded(child: picture), SizedBox(width: 280, child: side)])
            : Column(children: [Expanded(flex: 3, child: picture), Expanded(flex: 2, child: side)]),
      ),
    );
  }
}
