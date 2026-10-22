// A pocket or profile never gets a V-bit by default: phobiCCCP leaves an
// offset toolpath with a V-bit out (its radius is far wider than it cuts).
import 'package:flutter_test/flutter_test.dart';
import 'package:phobicccp/model.dart';
import 'package:phobicccp/ui/send_dialog.dart';

void main() {
  const vbit = Tool(number: 301, name: 'V 90', kind: 'vbit', diameter: 12.7, angle: 90, materials: ['softwood']);
  const engraver = Tool(number: 501, name: 'Engraver', kind: 'engraver', diameter: 3.175, angle: 30, materials: ['softwood']);
  const endmill = Tool(number: 201, name: '1/4"', kind: 'endmill', diameter: 6.35, materials: ['softwood']);
  const other = Tool(number: 900, name: 'Odd', kind: 'other', diameter: 3, materials: ['softwood']);
  test('V-bits and engravers are pointed, end mills are not', () {
    expect(vbit.isVee, isTrue);
    expect(engraver.isVee, isTrue);
    expect(endmill.isVee, isFalse);
  });
  test('no V-bit as the default for a pocket', () {
    const lib = ToolLibrary(tools: [vbit, engraver, other]);
    expect(lib.defaultTool('softwood', 'pocket'), 900);
    expect(const ToolLibrary(tools: [vbit]).defaultTool('softwood', 'profile'), isNull);
    expect(const ToolLibrary(tools: [vbit, endmill]).defaultTool('softwood', 'engrave'), 301);
  });
  test('the send dialog warns only where phobiCCCP leaves the toolpath out', () {
    ToolpathSpec tp(String type, String side, {bool enabled = true}) =>
        ToolpathSpec(name: 't', type: type, shapes: const ['a'], tool: 301, depth: 1, side: side, enabled: enabled);
    expect(veeLeftOut(tp('pocket', 'outside'), vbit), isTrue);
    expect(veeLeftOut(tp('profile', 'inside'), vbit), isTrue);
    expect(veeLeftOut(tp('profile', 'on'), vbit), isFalse);
    expect(veeLeftOut(tp('engrave', 'on'), vbit), isFalse);
    expect(veeLeftOut(tp('pocket', 'outside', enabled: false), vbit), isFalse);
    expect(veeLeftOut(tp('pocket', 'outside'), endmill), isFalse);
  });
}
