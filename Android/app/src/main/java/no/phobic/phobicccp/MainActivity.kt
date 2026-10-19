package no.phobic.phobicccp

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.viewmodel.compose.viewModel
import no.phobic.phobicccp.ui.AppViewModel
import no.phobic.phobicccp.ui.EditorScreen
import no.phobic.phobicccp.ui.HomeScreen
import no.phobic.phobicccp.ui.PhobiTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            PhobiTheme {
                val vm: AppViewModel = viewModel()
                var editing by rememberSaveable { mutableStateOf<String?>(null) }
                val id = editing
                if (id == null) {
                    HomeScreen(vm, onOpen = { editing = it })
                } else {
                    BackHandler { editing = null }
                    EditorScreen(vm, id, onClose = { editing = null })
                }
            }
        }
    }
}
