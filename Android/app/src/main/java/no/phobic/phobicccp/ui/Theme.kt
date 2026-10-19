package no.phobic.phobicccp.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// The desktop app's colours: its dark shop theme, the icon's red and gold.
object Brand {
    val Red = Color(0xFFCC0000)
    val Gold = Color(0xFFFFD700)
    val Amber = Color(0xFFF2A536)
    val Background = Color(0xFF16181D)
    val Surface = Color(0xFF2A2E36)
    val SurfaceHigh = Color(0xFF333945)
    val Text = Color(0xFFD8DCE4)
    val TextDim = Color(0xFF8B93A1)
    val Good = Color(0xFF4CAF50)
    val Blue = Color(0xFF4A90D9)
}

private val Colors = darkColorScheme(
    primary = Brand.Red,
    onPrimary = Color.White,
    secondary = Brand.Gold,
    onSecondary = Color.Black,
    tertiary = Brand.Amber,
    background = Brand.Background,
    onBackground = Brand.Text,
    surface = Brand.Background,
    onSurface = Brand.Text,
    surfaceVariant = Brand.Surface,
    onSurfaceVariant = Brand.TextDim,
    surfaceContainer = Brand.Surface,
    surfaceContainerHigh = Brand.SurfaceHigh,
    surfaceContainerHighest = Brand.SurfaceHigh,
    surfaceContainerLow = Brand.Surface,
    outline = Color(0xFF4A505C),
    error = Color(0xFFFF6B6B),
)

@Composable
fun PhobiTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors, content = content)
}
