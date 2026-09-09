package com.orkgabb.m54tuner

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.orkgabb.m54tuner.ui.MainScreen
import com.orkgabb.m54tuner.ui.theme.M54TunerTheme

class MainActivity : ComponentActivity() {

    private val viewModel: MainViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            M54TunerTheme {
                MainScreen(viewModel = viewModel)
            }
        }
    }
}
