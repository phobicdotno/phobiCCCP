// The desktop app's colours: its dark shop theme, the icon's red and gold.

import 'package:flutter/material.dart';

class Brand {
  static const red = Color(0xFFCC0000);
  static const gold = Color(0xFFFFD700);
  static const amber = Color(0xFFF2A536);
  static const background = Color(0xFF16181D);
  static const surface = Color(0xFF2A2E36);
  static const surfaceHigh = Color(0xFF333945);
  static const text = Color(0xFFD8DCE4);
  static const textDim = Color(0xFF8B93A1);
  static const good = Color(0xFF4CAF50);
  static const blue = Color(0xFF4A90D9);
  static const stock = Color(0xFF3B3226);
  static const stockEdge = Color(0xFF6B5B45);
}

ThemeData phobiTheme() {
  final scheme = const ColorScheme.dark(
    primary: Brand.red,
    onPrimary: Colors.white,
    secondary: Brand.gold,
    onSecondary: Colors.black,
    tertiary: Brand.amber,
    surface: Brand.background,
    onSurface: Brand.text,
    onSurfaceVariant: Brand.textDim,
    surfaceContainerLowest: Brand.background,
    surfaceContainerLow: Brand.surface,
    surfaceContainer: Brand.surface,
    surfaceContainerHigh: Brand.surfaceHigh,
    surfaceContainerHighest: Brand.surfaceHigh,
    outline: Color(0xFF4A505C),
    error: Color(0xFFFF6B6B),
  );
  return ThemeData(
    colorScheme: scheme,
    scaffoldBackgroundColor: Brand.background,
    useMaterial3: true,
    appBarTheme: const AppBarTheme(backgroundColor: Brand.background, foregroundColor: Brand.text),
    cardTheme: const CardThemeData(color: Brand.surface),
  );
}

/// Wide enough for the pad layout: drawing and panels side by side.
bool isWide(BuildContext context) => MediaQuery.sizeOf(context).width >= 840;
