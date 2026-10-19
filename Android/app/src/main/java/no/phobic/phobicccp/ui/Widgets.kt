package no.phobic.phobicccp.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import no.phobic.phobicccp.model.Stock
import no.phobic.phobicccp.model.ToolLibrary
import java.util.Locale

/** A millimetre (or count) field that only reports numbers it can read. */
@Composable
fun NumberField(
    label: String,
    value: Double,
    onValue: (Double) -> Unit,
    modifier: Modifier = Modifier,
    min: Double = -10000.0,
    max: Double = 10000.0,
) {
    var text by remember(value) { mutableStateOf(fmt(value)) }
    val parsed = text.replace(',', '.').toDoubleOrNull()
    val bad = parsed == null || parsed < min || parsed > max
    OutlinedTextField(
        value = text,
        onValueChange = {
            text = it
            it.replace(',', '.').toDoubleOrNull()?.takeIf { v -> v in min..max }?.let(onValue)
        },
        label = { Text(label) },
        isError = bad,
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        modifier = modifier,
    )
}

fun fmt(v: Double): String =
    if (v == Math.rint(v) && kotlin.math.abs(v) < 1e9) v.toLong().toString()
    else String.format(Locale.US, "%.3f", v).trimEnd('0').trimEnd('.')

/** One choice out of a few, as chips. */
@Composable
fun <T> ChipChoice(options: List<T>, selected: T, label: (T) -> String, onSelect: (T) -> Unit) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        for (o in options)
            FilterChip(selected = o == selected, onClick = { onSelect(o) }, label = { Text(label(o)) })
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun <T> Dropdown(label: String, options: List<T>, selected: T?, text: (T) -> String, onSelect: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { open = it }) {
        OutlinedTextField(
            value = selected?.let(text) ?: "",
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = open) },
            modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable).fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (o in options)
                DropdownMenuItem(text = { Text(text(o)) }, onClick = {
                    onSelect(o)
                    open = false
                })
        }
    }
}

/** Stock size and material, used by the new-design and stock dialogs. */
@Composable
fun StockFields(stock: Stock, material: String, tools: ToolLibrary, onStock: (Stock) -> Unit, onMaterial: (String) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NumberField("Width X", stock.width, { onStock(stock.copy(width = it)) }, Modifier.weight(1f), 1.0)
            NumberField("Height Y", stock.height, { onStock(stock.copy(height = it)) }, Modifier.weight(1f), 1.0)
        }
        NumberField("Thickness Z", stock.thickness, { onStock(stock.copy(thickness = it)) }, Modifier.fillMaxWidth(), 0.1, 1000.0)
        val mats = tools.materials.map { it.id }.ifEmpty { listOf("softwood") }
        Dropdown("Material", mats, material, { tools.materialName(it) }, onMaterial)
        if (tools.materials.isEmpty())
            Text("The materials come from phobiCCCP's tool library once the phone has found it.",
                modifier = Modifier.padding(top = 2.dp))
    }
}
