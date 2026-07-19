package com.anatdx.yukisu.ui.component

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.*
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.R
import com.anatdx.yukisu.ui.theme.isExpressiveUi

@Preview
@Composable
fun AboutCard() {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        shape = if (isExpressiveUi) MaterialTheme.shapes.extraLarge else RoundedCornerShape(8.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(24.dp)
        ) {
            AboutCardContent()
        }
    }
}

@Composable
fun AboutDialog(dismiss: () -> Unit) {
    Dialog(
        onDismissRequest = { dismiss() }
    ) {
        Box(modifier = Modifier.clickHapticFeedback()) {
            AboutCard()
        }
    }
}

@Composable
private fun AboutCardContent() {
    Column(
        modifier = Modifier.fillMaxWidth()
    ) {
        Row {
            Image(
                painter = painterResource(id = R.drawable.ic_about_yukisu),
                contentDescription = "icon",
                modifier = Modifier.size(40.dp)
            )

            Spacer(modifier = Modifier.width(12.dp))

            Column {

                Text(
                    stringResource(id = R.string.app_name),
                    style = if (isExpressiveUi) {
                        MaterialTheme.typography.titleLarge.copy(fontWeight = FontWeight.Normal)
                    } else {
                        MaterialTheme.typography.titleSmall
                    },
                    fontSize = if (isExpressiveUi) 22.sp else 18.sp
                )
                Text(
                    BuildConfig.VERSION_NAME,
                    style = MaterialTheme.typography.bodySmall,
                    fontSize = 14.sp
                )

                Spacer(modifier = Modifier.height(8.dp))

                val annotatedString = AnnotatedString.fromHtml(
                    htmlString = stringResource(
                        id = R.string.about_source_code,
                        "<b><a href=\"https://github.com/Anatdx/YukiSU\">GitHub</a></b>",
                        "<b><a href=\"https://t.me/manosaba\">Telegram</a></b>"
                    ),
                    linkStyles = TextLinkStyles(
                        style = SpanStyle(
                            color = MaterialTheme.colorScheme.primary,
                            textDecoration = TextDecoration.Underline
                        ),
                        pressedStyle = SpanStyle(
                            color = MaterialTheme.colorScheme.primary,
                            background = MaterialTheme.colorScheme.secondaryContainer,
                            textDecoration = TextDecoration.Underline
                        )
                    )
                )
                Text(
                    text = annotatedString,
                    style = TextStyle(
                        fontSize = 14.sp
                    )
                )
            }
        }
    }
}
