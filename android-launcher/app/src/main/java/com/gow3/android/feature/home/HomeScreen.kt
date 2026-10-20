package com.gow3.android.feature.home

import com.gow3.android.ui.components.ScreenColumn
import com.gow3.android.ui.components.formatBytes

import com.gow3.android.core.settings.GamePrefs
import com.gow3.android.core.settings.Gow3Settings
import com.gow3.android.data.CatalogProblem
import com.gow3.android.data.Gow3Data
import com.gow3.android.data.archive.SaveArchive
import com.gow3.android.data.driver.DriverEntry
import com.gow3.android.data.driver.DriverManager
import com.gow3.android.data.model.GameIdentity
import com.gow3.android.data.model.GameIdentityState
import com.gow3.android.data.model.MobilePreset
import com.gow3.android.data.model.MobilePresets
import com.gow3.android.data.session.ImportResult
import com.gow3.android.data.session.LosslessDllManager
import com.gow3.android.data.session.RemoveResult
import com.gow3.android.data.session.SaveManager
import com.gow3.android.ui.components.LauncherColors


import android.content.Context
import android.content.SharedPreferences
import android.widget.Toast
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.gow3.android.R
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Home 启动台（change redesign-android-launcher-home）：按「游戏区 → 配置区 →
 * 启动按钮 → 用户/存档区」四区构图，启动按钮以上在常见竖屏手机首屏内完整可见。
 * 目录选择不在本页——首启在专门的选择页完成，后续修改在 Settings 存储分类，
 * 身份卡异常态负责把玩家引导过去。窄屏单列，宽屏（[wide]）双栏。
 */
@Composable
internal fun HomeScreen(
    onStartGame: () -> Unit,
    losslessDlls: LosslessDllManager,
    onGetFromSteam: ((Boolean, String) -> Unit) -> Unit,
    onOpenStorageCategory: () -> Unit,
    onManageDrivers: () -> Unit,
    saves: SaveManager,
    wide: Boolean,
) {
    val context = LocalContext.current
    // 组合作用域预解析文案：事件回调/Toast 内不得调用 stringResource（非组合上下文）。
    val saveFailedMsg = stringResource(R.string.settings_save_failed)
    val driverPickFmt = stringResource(R.string.driver_pick_selected_fmt)
    val systemDriverName = stringResource(R.string.driver_card_system)
    var iniValues by remember { mutableStateOf(loadIni(context)) }
    var dllState by remember { mutableStateOf(losslessDlls.resolve()) }
    var gameDir by remember { mutableStateOf(currentGameDir(context)) }
    var updateDir by remember { mutableStateOf(currentUpdateDir(context)) }
    var notice by remember { mutableStateOf<String?>(null) }
    // 游戏身份状态：catalog 或 CatalogProblem（不再吞错），驱动 Home 身份卡三态。
    var identity by remember { mutableStateOf<GameIdentityState>(GameIdentityState.Loading) }
    var identityRevision by remember { mutableIntStateOf(0) }
    // uncharted_android: the collection's folder gets a game picker instead of the God of War III
    // presets, and the identity card names the chosen game.
    val uncharted = remember(gameDir) {
        com.gow3.android.data.UnchartedGames.isCollection(gameDir?.let { java.io.File(it) })
    }
    val ucGame = remember(gameDir, iniValues) {
        com.gow3.android.data.UnchartedGames.selected(gameDir?.let { java.io.File(it) }, iniValues)
    }
    // 驱动卡片的重扫信号：回到前台 / 目录变化 / 本页写入选择时都会递增。
    var driverRevision by remember { mutableIntStateOf(0) }
    // 驱动选择面板的数据：每次 revision 递增（前台/目录变化/本页写入）重扫磁盘。
    var driverEntries by remember { mutableStateOf(emptyList<DriverEntry>()) }
    // 用户/存档区占位的只读摘要：当前存档目录是否有存档。
    var hasSaves by remember { mutableStateOf(false) }
    val prefs = remember(context) { GamePrefs(context) }

    fun refreshDll() {
        dllState = losslessDlls.resolve()
    }

    fun toast(message: String) {
        notice = message
        Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
    }

    // 玩家可能在游戏运行期间把 Lossless.dll 放到游戏目录旁，因此每次 Home 回到前台
    // 时都会从磁盘重新读取状态。
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner, prefs) {
        fun refreshSettings() {
            gameDir = currentGameDir(context)
            updateDir = currentUpdateDir(context)
            iniValues = loadIni(context)
            identityRevision++
            driverRevision++
            hasSaves = SaveArchive.hasSaves(saves.savedataDir())
        }
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == "game_dir" || key == "update_dir") refreshSettings()
        }
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                refreshDll()
                refreshSettings()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        prefs.raw.registerOnSharedPreferenceChangeListener(listener)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            prefs.raw.unregisterOnSharedPreferenceChangeListener(listener)
        }
    }

    LaunchedEffect(identityRevision) {
        identity = GameIdentityState.Loading
        identity = withContext(Dispatchers.IO) {
            GameIdentity.load(gameDir, updateDir, context.assets::open)
        }
    }

    LaunchedEffect(driverRevision) {
        driverEntries = withContext(Dispatchers.IO) { DriverManager.scan(context.filesDir) }
    }

    fun saveUpdates(updates: Map<String, String>): Boolean {
        return if (Gow3Settings.update(gow3IniFile(context), updates)) {
            iniValues = loadIni(context)
            true
        } else {
            Toast.makeText(context, saveFailedMsg, Toast.LENGTH_SHORT).show()
            false
        }
    }

    fun applyPreset(preset: MobilePreset) {
        val catalog = (identity as? GameIdentityState.Ready)?.catalog ?: return
        if (preset !in MobilePresets.available(catalog)) return
        saveUpdates(MobilePresets.updates(preset, catalog, loadIni(context)))
    }

    // 跳过动画快捷开关与预设共用同一份目录解析；目录未就绪时开关自身置灰。
    val homeCatalog = (identity as? GameIdentityState.Ready)?.catalog

    /** 驱动选择：写 `gpu_driver=` 键（系统驱动写 auto），与 run-bionic.sh 的解析契约一致。 */
    fun selectDriver(id: String?, displayName: String) {
        if (saveUpdates(mapOf(DriverManager.INI_KEY to (id ?: DriverManager.SYSTEM_VALUE)))) {
            toast(driverPickFmt.format(displayName))
        }
        driverRevision++
    }

    // 启动前置不再静默失败（spec：Game prerequisites are missing）：身份卡未就绪时
    // 给出与失败原因一致的引导提示，而不是无反应。
    // 启动失败文案随 identity 派生（组合作用域解析，回调内只引用 val）。
    val launchProblemMsg = (identity as? GameIdentityState.Failed)
        ?.let { stringResource(GameIdentity.problemMessageRes(it.problem)) }

    fun launch() {
        when (val state = identity) {
            is GameIdentityState.Ready -> onStartGame()
            is GameIdentityState.Failed -> launchProblemMsg?.let(::toast)
            GameIdentityState.Loading -> {}
        }
    }

    // —— 四区内容（窄屏/宽屏共用同一组组件） ——
    if (wide) {
        // 宽屏双栏（spec：Home switches to a two-column layout on wide screens）：
        // 左栏 = 身份卡 + 启动按钮，右栏 = 用户/存档区 + 预设 + 驱动；两栏独立滚动，
        // 身份卡与启动按钮不因右栏滚动而移出视野。
        Row(
            modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp, vertical = 24.dp),
            horizontalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Column(
                modifier = Modifier.weight(1f).verticalScroll(rememberScrollState()),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                GameIdentityCard(identity, onOpenStorageCategory, ucGame)
                LaunchButton(::launch)
            }
            Column(
                modifier = Modifier.weight(1f).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                UserSavesZone(hasSaves)
                if (uncharted) UnchartedGamePicker(iniValues) { saveUpdates(mapOf("uc_game" to it)) }
                else PresetSegmented(identity, iniValues, ::applyPreset)
                DriverSelector(
                    identity, iniValues, driverEntries,
                    // 面板只回传 id：展示名从扫描结果解析（系统驱动回退到本地化文案）。
                    onSelect = { id ->
                        val name = id?.let { picked -> driverEntries.firstOrNull { it.id == picked }?.name }
                            ?: systemDriverName
                        selectDriver(id, name)
                    },
                    onManageDrivers,
                )
                FpsLimitRow(iniValues) { saveUpdates(mapOf("fps_limit" to it)) }
                SkipAnimationsCard(homeCatalog, iniValues, ::saveUpdates)
                // gow3port: LSFG 运行时能力已废除（Bionic 迁移，见 openspec change
                // migrate-android-runtime-to-bionic）。「Steam 下载 LSFG / 导入 LSFG」入口
                // 保留代码、仅隐藏（不删除文件、不产生帧生成行为），供未来插帧方案复用。
                FrameGenerationSection(losslessDlls, dllState, iniValues, context) { iniValues = loadIni(context) }
            }
        }
    } else {
        ScreenColumn(horizontalAlignment = Alignment.CenterHorizontally) {
            Spacer(Modifier.height(28.dp))
            GameIdentityCard(identity, onOpenStorageCategory, ucGame)
            if (uncharted) UnchartedGamePicker(iniValues) { saveUpdates(mapOf("uc_game" to it)) }
            else PresetSegmented(identity, iniValues, ::applyPreset)
            DriverSelector(
                identity, iniValues, driverEntries,
                // 面板只回传 id：展示名从扫描结果解析（系统驱动回退到本地化文案）。
                onSelect = { id ->
                    val name = id?.let { picked -> driverEntries.firstOrNull { it.id == picked }?.name }
                        ?: systemDriverName
                    selectDriver(id, name)
                },
                onManageDrivers,
            )
            FpsLimitRow(iniValues) { saveUpdates(mapOf("fps_limit" to it)) }
            SkipAnimationsCard(homeCatalog, iniValues, ::saveUpdates)
            Spacer(Modifier.height(6.dp))
            LaunchButton(::launch)
            Spacer(Modifier.height(12.dp))
            UserSavesZone(hasSaves)
            // gow3port: LSFG 运行时能力已废除（Bionic 迁移，见 openspec change
            // migrate-android-runtime-to-bionic）。「Steam 下载 LSFG / 导入 LSFG」入口保留
            // 代码、仅隐藏（不删除文件、不产生帧生成行为），供未来插帧方案复用。恢复时打开。
            FrameGenerationSection(losslessDlls, dllState, iniValues, context) { iniValues = loadIni(context) }
            Spacer(Modifier.height(20.dp))
        }
    }
}

/**
 * FRAME GENERATION 区块（change add-native-frame-generation）：DLL 供给 + 帧生成
 * 开关/倍率。开关与倍率写 `gow3.ini` 的 `framegen_*` 键（FrameGenSettings 整行
 * 保留语义），下次启动游戏生效；DLL 供给行为原样保留。
 */
@Composable
private fun FrameGenerationSection(
    losslessDlls: LosslessDllManager,
    dllState: com.gow3.android.data.session.LosslessDllState,
    iniValues: Map<String, String>,
    context: Context,
    onIniChanged: () -> Unit,
) {
    val cfg = com.gow3.android.core.settings.FrameGenSettings.read(gow3IniFile(context))
    val dllInstalled = dllState is com.gow3.android.data.session.LosslessDllState.Installed
    val frameGenSaveFailedMsg = stringResource(R.string.frame_gen_save_failed)
    FrameGenerationCard(
        enabled = cfg.enabled,
        dllInstalled = dllInstalled,
        onChange = { on ->
            // 开关只写 framegen_enabled；倍率在游戏内抽屉调。无 DLL 时开关被禁用，
            // 不会产生运行时行为（spec「无有效 DLL 时明确不可用」）。
            val file = gow3IniFile(context)
            if (com.gow3.android.core.settings.FrameGenSettings.save(
                    file, com.gow3.android.core.settings.FrameGenSettings.Config(on, cfg.multiplier, cfg.flowScale))
            ) {
                onIniChanged()
            } else {
                android.widget.Toast.makeText(
                    context, frameGenSaveFailedMsg,
                    android.widget.Toast.LENGTH_SHORT).show()
            }
        },
    )
}


private fun gow3IniFile(context: Context): File = Gow3Data.config(context)

private fun loadIni(context: Context): Map<String, String> = Gow3Settings.read(gow3IniFile(context))

private fun currentGameDir(context: Context): String? =
    GamePrefs(context).gameDir()

private fun currentUpdateDir(context: Context): String? =
    GamePrefs(context).updateDir()

/**
 * 游戏身份卡（spec：Home shows a game identity card with health states）：健康时展示
 * 标题 + TITLE ID/版本徽章 + 补丁状态；未选目录/无法读取时展示原因并引导到 Settings
 * 存储分类。加载中呈骨架态且不阻塞页面其它区域。
 */
@Composable
private fun GameIdentityCard(
    state: GameIdentityState,
    onOpenStorage: () -> Unit,
    ucGame: com.gow3.android.data.UnchartedGames.Game? = null,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(LauncherColors.surface)
            .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.5f)), RoundedCornerShape(14.dp))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        when (state) {
            GameIdentityState.Loading -> {
                // 加载骨架：保持卡片高度稳定，避免解析完成时页面跳动。
                Box(Modifier.fillMaxWidth().height(22.dp).clip(RoundedCornerShape(6.dp)).background(LauncherColors.navSurface))
                Box(Modifier.fillMaxWidth(0.5f).height(12.dp).clip(RoundedCornerShape(6.dp)).background(LauncherColors.navSurface))
                Text(stringResource(R.string.mods_loading), color = LauncherColors.textMuted, fontSize = 11.sp)
            }
            is GameIdentityState.Ready -> {
                val catalog = state.catalog
                val title = if (ucGame != null) "UNCHARTED" else stringResource(R.string.title_god_of_war_iii)
                val subtitle = ucGame?.title?.substringAfter(": ", ucGame.title)?.uppercase()
                    ?: stringResource(R.string.title_remastered)
                Text(title, color = LauncherColors.textPrimary, fontFamily = FontFamily.Serif, fontSize = 20.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.8.sp)
                Text(subtitle, color = LauncherColors.textMuted, fontSize = 11.sp, fontWeight = FontWeight.Medium, letterSpacing = 2.sp)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    IdentityBadge(catalog.titleId)
                    IdentityBadge("VER " + catalog.appVersion)
                }
                Text(
                    stringResource(R.string.home_identity_patches_loaded_fmt, catalog.patches.size),
                    color = LauncherColors.textMuted, fontSize = 11.sp, lineHeight = 16.sp,
                )
            }
            is GameIdentityState.Failed -> {
                val problem = state.problem
                if (problem != CatalogProblem.NO_GAME) {
                    // 防御态之外的失败给出标题 + 原因；NO_GAME 只给引导（正常链路到不了这里）。
                    Text(stringResource(R.string.home_identity_problem_title), color = LauncherColors.textPrimary, fontFamily = FontFamily.Serif, fontSize = 15.sp, fontWeight = FontWeight.Bold)
                }
                Text(
                    stringResource(GameIdentity.problemMessageRes(problem)),
                    color = LauncherColors.textMuted, fontSize = 12.sp, lineHeight = 17.sp,
                )
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(LauncherColors.navSurface)
                        .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.5f)), RoundedCornerShape(8.dp))
                        .clickable(onClick = onOpenStorage)
                        .padding(vertical = 12.dp, horizontal = 12.dp),
                    horizontalArrangement = Arrangement.Center,
                ) {
                    Text(
                        stringResource(R.string.home_identity_open_storage),
                        color = LauncherColors.gold, fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold, letterSpacing = 0.4.sp,
                    )
                }
            }
        }
    }
}

/** 身份徽章：金色描边小胶囊（TITLE ID / 版本号）。 */
@Composable
private fun IdentityBadge(text: String) {
    Text(
        text,
        color = LauncherColors.gold,
        fontSize = 11.sp,
        fontWeight = FontWeight.SemiBold,
        letterSpacing = 0.6.sp,
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(LauncherColors.navSurface)
            .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.45f)), RoundedCornerShape(6.dp))
            .padding(horizontal = 8.dp, vertical = 4.dp),
    )
}

@Composable
private fun LaunchButton(onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .height(84.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(Brush.linearGradient(listOf(Color(0xFFB8172B), Color(0xFF8C0D1A), Color(0xFFB8172B))))
            .border(BorderStroke(1.dp, LauncherColors.gold), RoundedCornerShape(12.dp))
            .clickable(onClick = onClick)
            .padding(vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(stringResource(R.string.launch_button), color = LauncherColors.textPrimary, fontFamily = FontFamily.Serif, fontSize = 30.sp, fontWeight = FontWeight.Black, letterSpacing = 1.3.sp)
        Text(stringResource(R.string.start_game_button), color = LauncherColors.textPrimary, fontSize = 10.sp, fontWeight = FontWeight.Medium, letterSpacing = 1.5.sp)
    }
}

/**
 * 用户/存档区（spec：Home provides a user and saves zone below the launch button）。
 * 本 change 只落地占位：自动生成的默认头像（按用户决定，不允许玩家自选图片）+ 只读
 * 存档摘要行；多用户切换、存档面板与导出/导入的核心逻辑随
 * add-android-launcher-user-profiles 在本区域落地。
 */
@Composable
private fun UserSavesZone(hasSaves: Boolean) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(LauncherColors.surface)
            .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.35f)), RoundedCornerShape(14.dp))
            .padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // 默认头像：首字母彩色圆，不做图片选择器（用户决定：图像不允许玩家自选）。
        Box(
            modifier = Modifier
                .size(40.dp)
                .clip(RoundedCornerShape(20.dp))
                .background(Brush.linearGradient(listOf(Color(0xFFB8172B), Color(0xFF6E0E1A))))
                .border(BorderStroke(1.dp, LauncherColors.gold.copy(alpha = 0.5f)), RoundedCornerShape(20.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Text("H", color = LauncherColors.textPrimary, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        }
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.user_zone_default_name), color = LauncherColors.textPrimary, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Text(stringResource(R.string.user_zone_title), color = LauncherColors.gold, fontSize = 9.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.8.sp)
            }
            Text(
                stringResource(if (hasSaves) R.string.user_zone_saves_ready else R.string.user_zone_saves_empty),
                color = LauncherColors.textMuted, fontSize = 11.sp,
            )
            Text(
                stringResource(R.string.user_zone_note),
                color = LauncherColors.textMuted, fontSize = 10.sp, lineHeight = 14.sp,
            )
        }
    }
}
