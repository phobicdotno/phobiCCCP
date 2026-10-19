// phobiCCCP's own look, taken from the desktop program (src/main.cpp
// applyDarkTheme, the header and panel style sheets in mainwindow.cpp, the
// canvas colours in canvas.cpp): Fusion dark, Inter, amber accents.

import 'package:flutter/material.dart';

class C {
  // Palette (main.cpp)
  static const window = Color(0xFF1E2127);
  static const base = Color(0xFF16181D);
  static const text = Color(0xFFD8DCE4);
  static const disabled = Color(0xFF6A717D);
  static const amber = Color(0xFFF2A536);
  static const button = Color(0xFF2A2E36);
  static const hover = Color(0xFF333945);
  static const checked = Color(0xFF3A4150);

  // Header, panels, cards (mainwindow.cpp, toolpathform.cpp, toolpathpanel.cpp)
  static const border = Color(0xFF2A2E36);
  static const panelHead = Color(0xFF23262E);
  static const card = Color(0xFF23262E);
  static const cardBorder = Color(0xFF2F343D);
  static const modeOn = Color(0xFF2F343D);
  static const modeText = Color(0xFFA3ABB8);
  static const label = Color(0xFFAAB1BD);
  static const sub = Color(0xFF8B93A1);
  static const fileName = Color(0xFFC5CAD3);
  static const nameOn = Color(0xFFE6E9EF);
  static const selCard = Color(0xFF3A3122);
  static const blue = Color(0xFF4A90D9);
  static const stop = Color(0xFF8B2A2A);

  // Canvas (canvas.cpp)
  static const viewBg = Color(0xFF15171C);
  static const boardFill = Color(0xFF21242B);
  static const boardEdge = Color(0xFF4A5260);
  static const gridMinor = Color(0xFF282C35);
  static const gridMajor = Color(0xFF343A46);
  static const axisX = Color(0xFF8A4A4A);
  static const axisY = Color(0xFF4A7A4A);
  static const element = Color(0xFFD8DCE4);
  static const hoverLine = Color(0xFF9FC8F2);
  static const preview = Color(0xFF5CC88A);
  static const halo = Color(0xFFFFA52A);

  // Logo
  static const red = Color(0xFFCC0000);
  static const gold = Color(0xFFFFD700);
}

const kFont = 'Inter';
const kMono = 'DejaVuSansMono';

ThemeData phobiTheme() {
  const scheme = ColorScheme.dark(
    primary: C.amber,
    onPrimary: Color(0xFF1A1A1A),
    secondary: C.amber,
    onSecondary: Color(0xFF1A1A1A),
    surface: C.window,
    onSurface: C.text,
    onSurfaceVariant: C.label,
    surfaceContainerLowest: C.base,
    surfaceContainerLow: C.window,
    surfaceContainer: C.window,
    surfaceContainerHigh: C.window,
    surfaceContainerHighest: C.button,
    outline: C.cardBorder,
    outlineVariant: C.border,
    error: Color(0xFFE05050),
  );
  final shape4 = RoundedRectangleBorder(borderRadius: BorderRadius.circular(4));
  final shape6 = RoundedRectangleBorder(borderRadius: BorderRadius.circular(6));
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    fontFamily: kFont,
    scaffoldBackgroundColor: C.window,
    canvasColor: C.window,
    splashFactory: NoSplash.splashFactory,
    highlightColor: C.hover,
    hoverColor: C.hover,
    visualDensity: VisualDensity.standard,
    textTheme: const TextTheme(
      bodyMedium: TextStyle(fontSize: 13, color: C.text),
      bodyLarge: TextStyle(fontSize: 14, color: C.text),
      bodySmall: TextStyle(fontSize: 12, color: C.label),
      titleMedium: TextStyle(fontSize: 14, color: C.text),
      labelLarge: TextStyle(fontSize: 13, color: C.text),
    ),
    dialogTheme: DialogThemeData(
      backgroundColor: C.window,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6), side: const BorderSide(color: C.cardBorder)),
      titleTextStyle: const TextStyle(fontFamily: kFont, fontSize: 15, fontWeight: FontWeight.w600, color: C.text),
      contentTextStyle: const TextStyle(fontFamily: kFont, fontSize: 13, color: C.text),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
          backgroundColor: C.amber, foregroundColor: const Color(0xFF16181D), shape: shape6, minimumSize: const Size(88, 36),
          textStyle: const TextStyle(fontFamily: kFont, fontSize: 14, fontWeight: FontWeight.w600)),
    ),
    textButtonTheme: TextButtonThemeData(
      style: TextButton.styleFrom(
          backgroundColor: C.button, foregroundColor: C.text, shape: shape4, minimumSize: const Size(88, 34),
          textStyle: const TextStyle(fontFamily: kFont, fontSize: 13)),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
          backgroundColor: C.button, foregroundColor: C.text, shape: shape6, side: BorderSide.none,
          textStyle: const TextStyle(fontFamily: kFont, fontSize: 14)),
    ),
    inputDecorationTheme: InputDecorationTheme(
      isDense: true,
      filled: true,
      fillColor: C.base,
      contentPadding: const EdgeInsets.symmetric(horizontal: 6, vertical: 8),
      labelStyle: const TextStyle(color: C.label, fontSize: 13),
      border: OutlineInputBorder(borderRadius: BorderRadius.circular(2), borderSide: const BorderSide(color: C.cardBorder)),
      enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(2), borderSide: const BorderSide(color: C.cardBorder)),
      focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(2), borderSide: const BorderSide(color: C.amber)),
    ),
    checkboxTheme: CheckboxThemeData(
      fillColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.amber : Colors.transparent),
      checkColor: const WidgetStatePropertyAll(C.base),
      side: const BorderSide(color: C.disabled, width: 1.6),
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
    ),
    radioTheme: RadioThemeData(
      fillColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.amber : C.disabled),
    ),
    switchTheme: SwitchThemeData(
      thumbColor: const WidgetStatePropertyAll(Colors.white),
      trackColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? C.amber : C.checked),
      trackOutlineColor: const WidgetStatePropertyAll(Colors.transparent),
    ),
    popupMenuTheme: PopupMenuThemeData(
      color: C.window,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4), side: const BorderSide(color: C.cardBorder)),
      textStyle: const TextStyle(fontFamily: kFont, fontSize: 14, color: C.text),
      menuPadding: const EdgeInsets.symmetric(vertical: 4),
    ),
    dividerTheme: const DividerThemeData(color: C.border, thickness: 1, space: 1),
    scrollbarTheme: const ScrollbarThemeData(thumbColor: WidgetStatePropertyAll(C.checked)),
    tooltipTheme: TooltipThemeData(
      decoration: BoxDecoration(color: C.base, border: Border.all(color: C.cardBorder)),
      textStyle: const TextStyle(fontFamily: kFont, fontSize: 12, color: C.text),
    ),
    snackBarTheme: const SnackBarThemeData(backgroundColor: C.button, contentTextStyle: TextStyle(color: C.text)),
  );
}
