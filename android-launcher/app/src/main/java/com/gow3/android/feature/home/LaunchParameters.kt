package com.gow3.android.feature.home

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.gow3.android.R
import com.gow3.android.data.driver.DriverEntry
import com.gow3.android.data.driver.DriverManager
import com.gow3.android.data.model.GameIdentityState
import com.gow3.android.data.model.LaunchParams
import com.gow3.android.data.model.MobilePreset
import com.gow3.android.data.model.MobilePresets
import com.gow3.android.feature.settings.ParamInfoDialog
import com.gow3.android.ui.components.LauncherColors
import com.gow3.android.ui.components.SectionLabel

/**
 * 首页预设分段选择器（spec：Home preset selection is a one-tap segmented control）：
 * 分段呈现全部可用档位，当前生效档位高亮；单次点击立即写 `gow3.ini` 并更新高亮，
 * 不要求确认。`matching == null`（ini 被手动改出预设外）时全部档位不高亮并显示
 * 「已自定义」提示，仍可选择任一档位。catalog 未就绪时呈禁用/占位态。
 */
@Composable
internal fun PresetSegmented(
    identity: GameIdentityState,
    ini: Map<String, String>,
    apply: (MobilePreset) -> Unit,
) {
    val catalog = (identity as? GameIdentityState.Ready)?.catalog
    val presets = MobilePresets.available(catalog)
    val selected = MobilePresets.matching(catalog, ini)
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionLabel(stringResource(R.string.mobile_preset_title))
        when {
            identity is GameIdentityState.Loading -> DisabledBar(stringResource(R.string.mods_loading))
            presets.isEmpty() -> DisabledBar(stringResource(R.string.preset_disabled_no_catalog))
            else -> {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(10.dp))
                        .background(LauncherColors.navSurface)
                        .border(
                            BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.35f)),
                            RoundedCornerShape(10.dp)
                        ),
                ) {
                    presets.forEachIndexed { index, preset ->
                        val active = preset == selected
                        Text(
                            preset.resolution,
                            color = if (active) LauncherColors.textPrimary else LauncherColors.textMuted,
                            fontSize = 13.sp,
                            fontWeight = if (active) FontWeight.Bold else FontWeight.SemiBold,
                            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                            modifier = Modifier
                                .weight(1f)
                                .clip(RoundedCornerShape(10.dp))
                                .background(if (active) LauncherColors.crimson else Color.Transparent)
                                .clickable { apply(preset) }
                                .padding(vertical = 12.dp),
                        )
                        if (index < presets.lastIndex) {
                            Box(
                                Modifier
                                    .width(1.dp)
                                    .height(36.dp)
                                    .background(LauncherColors.gold.copy(alpha = 0.2f)),
                            )
                        }
                    }
                }
                // 说明行：命中预设展示其完整文案；被手动改参后展示「已自定义」。
                Text(
                    text = when {
                        selected != null -> stringResource(selected.labelRes)
                        else -> stringResource(R.string.preset_caption_custom)
                    },
                    color = if (selected != null) LauncherColors.textMuted else LauncherColors.gold.copy(
                        alpha = 0.8f
                    ),
                    fontSize = 11.sp,
                    lineHeight = 16.sp,
                )
            }
        }
    }
}

/**
 * uncharted_android: which game of the collection the LAUNCH button starts (`uc_game` in gow3.ini,
 * read by Gow3Prepare through UnchartedGames). One tap selects; it applies on the next launch.
 */
@Composable
internal fun UnchartedGamePicker(ini: Map<String, String>, select: (String) -> Unit) {
    val games = com.gow3.android.data.UnchartedGames.GAMES
    val current = ini["uc_game"]?.trim()?.lowercase()?.takeIf { key -> games.any { it.key == key } } ?: "uc3"
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionLabel("GAME")
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(LauncherColors.navSurface)
                .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.35f)), RoundedCornerShape(10.dp)),
        ) {
            games.forEachIndexed { index, game ->
                val active = game.key == current
                Text(
                    "Uncharted ${index + 1}",
                    color = if (active) LauncherColors.textPrimary else LauncherColors.textMuted,
                    fontSize = 13.sp,
                    fontWeight = if (active) FontWeight.Bold else FontWeight.SemiBold,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                    modifier = Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(10.dp))
                        .background(if (active) LauncherColors.crimson else Color.Transparent)
                        .clickable { select(game.key) }
                        .padding(vertical = 12.dp),
                )
                if (index < games.lastIndex) {
                    Box(Modifier.width(1.dp).height(36.dp).background(LauncherColors.gold.copy(alpha = 0.2f)))
                }
            }
        }
        val game = games.first { it.key == current }
        Text(
            game.title + " · " + game.status,
            color = LauncherColors.textMuted,
            fontSize = 11.sp,
            lineHeight = 16.sp,
        )
    }
}

@Composable
private fun DisabledBar(text: String) {
    Text(
        text,
        color = LauncherColors.textMuted, fontSize = 13.sp, lineHeight = 16.sp,
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .background(LauncherColors.navSurface)
            .border(
                BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.2f)),
                RoundedCornerShape(8.dp)
            )
            .padding(horizontal = 12.dp, vertical = 12.dp),
    )
}

/**
 * GPU 驱动选择入口 + 底部面板（spec：android-kotlin-launcher-driver-manager
 * 「首页展示与选择驱动」的底部面板形态）：面板列出「系统驱动」兜底与有效驱动包
 * （名称/版本元信息），失效包不出现（选中项失效时入口回退显示系统驱动）；选择写入
 * `gpu_driver=` 后关闭面板；「管理驱动」深链 Settings 驱动分类。无游戏目录时禁用。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun DriverSelector(
    identity: GameIdentityState,
    ini: Map<String, String>,
    entries: List<DriverEntry>,
    onSelect: (String?) -> Unit,
    onManageDrivers: () -> Unit,
) {
    // 无游戏目录（含未选目录/目录损坏）时禁用：沿用驱动Manager 的既有契约。
    val enabled = identity is GameIdentityState.Ready
    val valid = entries.filter { it.valid }
    // 指向失效/已删除包的选择按「系统驱动」回退展示（spec：驱动包失效后显示回退）。
    val selectedId = DriverManager.selectedId(ini)
    val selectedEntry = valid.firstOrNull { it.id == selectedId }
    var sheetOpen by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionLabel(stringResource(R.string.driver_card_title))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(LauncherColors.navSurface)
                .border(
                    BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.35f)),
                    RoundedCornerShape(8.dp)
                )
                .clickable(enabled = enabled) { sheetOpen = true }
                .padding(horizontal = 12.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val current =
                selectedEntry?.let { it.name + if (it.version.isBlank()) "" else "  ·  ${it.version}" }
                    ?: stringResource(R.string.driver_card_system)
            Text(
                current, modifier = Modifier.weight(1f),
                color = if (enabled) LauncherColors.textPrimary else LauncherColors.textMuted,
                fontSize = 13.sp, fontWeight = FontWeight.SemiBold
            )
            Text("▾", color = LauncherColors.textMuted)
        }
        Text(
            if (enabled) stringResource(R.string.driver_card_footer)
            else stringResource(R.string.driver_card_needs_game_folder),
            color = LauncherColors.textMuted, fontSize = 11.sp, lineHeight = 16.sp,
        )
    }
    if (sheetOpen) {
        ModalBottomSheet(
            onDismissRequest = { sheetOpen = false },
            containerColor = LauncherColors.surface,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                SectionLabel(stringResource(R.string.driver_sheet_title))
                Spacer(Modifier.height(8.dp))
                DriverOption(
                    title = stringResource(R.string.driver_card_system),
                    meta = null,
                    selected = selectedEntry == null,
                ) {
                    sheetOpen = false
                    onSelect(null)
                }
                valid.forEach { entry ->
                    DriverOption(
                        title = entry.name,
                        meta = listOfNotNull(
                            entry.version.takeIf { it.isNotBlank() },
                            entry.author.takeIf { it.isNotBlank() },
                        ).joinToString(" · ").ifBlank { null },
                        selected = selectedEntry?.id == entry.id,
                    ) {
                        sheetOpen = false
                        onSelect(entry.id)
                    }
                }
                Spacer(Modifier.height(8.dp))
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(1.dp)
                        .background(LauncherColors.gold.copy(alpha = 0.2f)),
                )
                // 管理驱动深链：下载/删除/导入仍位于 Settings 驱动分类，面板不重复提供。
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable {
                            sheetOpen = false
                            onManageDrivers()
                        }
                        .padding(vertical = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        stringResource(R.string.driver_sheet_manage),
                        color = LauncherColors.gold, fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold, letterSpacing = 0.4.sp,
                        modifier = Modifier.weight(1f),
                    )
                }
                Spacer(Modifier.height(24.dp))
            }
        }
    }
}

@Composable
private fun DriverOption(title: String, meta: String?, selected: Boolean, onPick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .clickable(onClick = onPick)
            .padding(horizontal = 4.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (selected) "●" else "○",
            color = if (selected) LauncherColors.gold else LauncherColors.textMuted,
            fontSize = 14.sp,
        )
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                title,
                color = LauncherColors.textPrimary,
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold
            )
            if (meta != null) {
                Text(meta, color = LauncherColors.textMuted, fontSize = 11.sp, lineHeight = 15.sp)
            }
        }
        if (selected) {
            Text("✓", color = LauncherColors.gold, fontSize = 14.sp)
        }
    }
}

/**
 * 帧率上限快捷下拉（change preset-resolution-only-fps-independent）：Home GPU 驱动选择下方
 * 就地编辑 `fps_limit`。档位按当前 `engine_fps` 约束（上限不超过引擎帧率，0=不限除外），
 * 与 Settings 配置页同一套取值域；写回 `gow3.ini` 的 `fps_limit=` 键，下次启动生效。
 */
@Composable
internal fun FpsLimitRow(ini: Map<String, String>, save: (String) -> Unit) {
    val param = LaunchParams.all.first { it.key == "fps_limit" }
    val options = LaunchParams.effectiveOptions(param, ini)
    var expanded by remember { mutableStateOf(false) }
    var showInfo by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            SectionLabel(stringResource(R.string.param_fps_limit))
            Text(
                "ⓘ",
                color = LauncherColors.gold,
                fontSize = 13.sp,
                lineHeight = 13.sp,
                modifier = Modifier
                    .clip(RoundedCornerShape(4.dp))
                    .clickable { showInfo = true }
                    .padding(2.dp),
            )
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(LauncherColors.navSurface)
                .border(
                    BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.35f)),
                    RoundedCornerShape(8.dp)
                )
                .clickable { expanded = true }
                .padding(horizontal = 12.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val current = LaunchParams.selectedOption(ini, param)
            Text(
                current?.let { stringResource(it.labelRes) } ?: LaunchParams.valueOf(ini, param),
                modifier = Modifier.weight(1f),
                color = LauncherColors.textPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Text("▾", color = LauncherColors.textMuted)
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEach { option ->
                DropdownMenuItem(
                    text = { Text(stringResource(option.labelRes)) },
                    onClick = {
                        expanded = false
                        save(option.value)
                    },
                )
            }
        }

        if (showInfo) {
            ParamInfoDialog(
                title = stringResource(R.string.param_fps_limit),
                message = stringResource(R.string.param_desc_fps_limit),
                onDismiss = { showInfo = false },
            )
        }
    }
}
