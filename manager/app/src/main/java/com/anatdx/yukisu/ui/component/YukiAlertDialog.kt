package com.anatdx.yukisu.ui.component

import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AlertDialogDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ProvideTextStyle
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.window.DialogProperties
import com.anatdx.yukisu.ui.theme.isExpressiveUi

/**
 * App-wide alert dialog entry point.
 *
 * Expressive UI uses the larger MD3E container and a regular-weight title while
 * the classic UI keeps Material 3's original AlertDialog defaults.
 */
@Composable
fun YukiAlertDialog(
    onDismissRequest: () -> Unit,
    confirmButton: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    dismissButton: (@Composable () -> Unit)? = null,
    icon: (@Composable () -> Unit)? = null,
    title: (@Composable () -> Unit)? = null,
    text: (@Composable () -> Unit)? = null,
    shape: Shape? = null,
    containerColor: Color? = null,
    iconContentColor: Color? = null,
    titleContentColor: Color? = null,
    textContentColor: Color? = null,
    tonalElevation: Dp? = null,
    properties: DialogProperties = DialogProperties(),
) {
    val expressive = isExpressiveUi
    val resolvedTitle = if (expressive && title != null) {
        @Composable {
            ProvideTextStyle(
                MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.Normal)
            ) {
                title()
            }
        }
    } else {
        title
    }

    AlertDialog(
        onDismissRequest = onDismissRequest,
        confirmButton = confirmButton,
        modifier = modifier.clickHapticFeedback(),
        dismissButton = dismissButton,
        icon = icon,
        title = resolvedTitle,
        text = text,
        shape = shape ?: if (expressive) {
            MaterialTheme.shapes.extraLarge
        } else {
            AlertDialogDefaults.shape
        },
        containerColor = containerColor ?: AlertDialogDefaults.containerColor,
        iconContentColor = iconContentColor ?: AlertDialogDefaults.iconContentColor,
        titleContentColor = titleContentColor ?: AlertDialogDefaults.titleContentColor,
        textContentColor = textContentColor ?: AlertDialogDefaults.textContentColor,
        tonalElevation = tonalElevation ?: AlertDialogDefaults.TonalElevation,
        properties = properties,
    )
}

/** Applies the MD3E dialog title typography to third-party dialog content. */
@Composable
fun YukiDialogTheme(content: @Composable () -> Unit) {
    if (isExpressiveUi) {
        val typography = MaterialTheme.typography
        MaterialTheme(
            colorScheme = MaterialTheme.colorScheme,
            shapes = MaterialTheme.shapes.copy(large = MaterialTheme.shapes.extraLarge),
            typography = typography.copy(
                headlineSmall = typography.titleLarge.copy(fontWeight = FontWeight.Normal),
                titleLarge = typography.titleLarge.copy(fontWeight = FontWeight.Normal),
            ),
            content = content,
        )
    } else {
        content()
    }
}
