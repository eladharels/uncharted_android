package com.gow3.android.feature.about

import com.gow3.android.ui.components.ActionButton

import com.gow3.android.ui.components.LauncherColors

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.gow3.android.R

/** 将 launcher 的 Uncharted 运行时日志（例如 android-game.log）导出为 zip。 */
@Composable
internal fun LogCard(
    hasLogs: Boolean,
    onExport: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(LauncherColors.surface)
            .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.5f)), RoundedCornerShape(14.dp))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Text(stringResource(R.string.log_runtime_logs), color = LauncherColors.gold, fontFamily = FontFamily.Serif, fontSize = 11.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.8.sp)
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(
                if (hasLogs) stringResource(R.string.logs_found) else stringResource(R.string.no_logs_yet),
                color = LauncherColors.textPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                stringResource(R.string.log_description),
                color = LauncherColors.textMuted,
                fontSize = 11.sp,
                lineHeight = 16.sp,
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            ActionButton(stringResource(R.string.log_export), hasLogs, Modifier.weight(1f), onExport)
        }
        Text(
            stringResource(R.string.log_export_footer),
            color = LauncherColors.textMuted,
            fontSize = 11.sp,
            lineHeight = 16.sp,
        )
    }
}