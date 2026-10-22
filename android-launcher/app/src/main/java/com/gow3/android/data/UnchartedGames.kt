package com.gow3.android.data

import java.io.File

/**
 * uncharted_android: the three games of Uncharted: The Nathan Drake Collection (PS4), one
 * executable each in the same folder, with the runtime options each was tested with on the PC
 * (zorde23/ucndc_pc launcher/uc_launcher.py: COMMON plus the game's own). The game is chosen with
 * `uc_game` in gow3.ini (uc1, uc2, uc3; default uc3, the most complete on the PC).
 */
object UnchartedGames {
    /** [status]: how far it is known to work in this Android port, shown under the picker. */
    data class Game(
        val key: String,
        val title: String,
        val executable: String,
        val env: Map<String, String>,
        val status: String = "Experimental",
    )

    /** Options every game starts with (uc_launcher.py COMMON). */
    private val COMMON = mapOf(
        "GOW3_READBACKS" to "2", "GOW3_FRAME_STATS" to "1", "GOW3_READBACK_ASYNC" to "1",
        "GOW3_LEAF_COPY" to "1", "GOW3_SCALAR_COPY_HLE" to "1", "GOW3_HLE_INVALIDATE" to "1",
        "GOW3_EAGER_SUBMIT" to "128", "GOW3_EAGER_SUBMIT_US" to "1000", "GOW3_ASYNC_SUBMIT" to "1",
        "GOW3_RING_COPY_THREAD" to "1", "GOW3_DEFERRED_SUBMIT" to "1", "GOW3_RING_DEDUP" to "1",
        "GOW3_READBACK_FAST" to "1", "GOW3_READBACK_FAST_WAIT" to "1", "GOW3_READBACK_FAST_SIDEQ" to "1",
        // Tested with stale readbacks off (ucndc gow3_settings.h); gow3_android defaults them on.
        "GOW3_STALE_READBACK" to "0",
        // uncharted_android: the six Naughty Dog job workers spin for work without ever blocking
        // during gameplay (probe.c patch_job_spin). On the PC that costs idle cores; on a phone
        // with eight, it takes the cores the renderer's threads need (12 FPS with the CPU at 70%).
        // They spin 100 times and block, in gameplay too, as the PC port runs Uncharted 3 (the
        // on-device profile shows them blocked in the futex most of the time, as intended).
        "GOW3_JOB_SPIN" to "100", "GOW3_JOB_SLEEP" to "1",
        // A scalar copy on the graphics queue is written by the draw recording thread in order
        // instead of draining the draw pipe first (ucndc's GOW3_HLE_INORDER_GFX, off on the PC),
        // and its wait for the Vulkan recording thread happens only while a deferred read of the
        // destination is pending (GOW3_HLE_PRECISE_WAIT). On the phone the drains at those
        // copies were a sixth of the command thread's time after the GPU-only ones stopped
        // draining; the pipe runs dry about seven times per frame otherwise.
        "GOW3_HLE_INORDER_GFX" to "1", "GOW3_HLE_PRECISE_WAIT" to "1",
        // The renderer's threads stay on the big cores (gow3_threads.h): the scheduler read
        // their burst-and-wait pattern as light load and ran them on the efficiency cores part
        // of the time. Galaxy S24 Ultra, Uncharted 1: 7-12 us per draw on the command thread
        // without, 5-7 with; frames of 2,000 draws from 50-67 ms to the 30 FPS cap. "2" in
        // uncharted-env.txt extends it to the whole process.
        "GOW3_BIG_CORES" to "1",
    )

    val GAMES = listOf(
        Game("uc1", "Uncharted: Drake's Fortune", "eboot.bin", COMMON + mapOf(
            "GOW3_COMPUTE_PIPE" to "1", "GOW3_LOW_IMAGE" to "1", "GOW3_AJM_END_RULES" to "1",
            "GOW3_EOP_TICK_WAIT" to "1", "GOW3_READBACK_SIDEQ" to "1",
        ), "Playable (Galaxy S24 Ultra)"),
        Game("uc2", "Uncharted 2: Among Thieves", "big2-ps4_Shipping.elf", COMMON + mapOf(
            "GOW3_HLE_INORDER" to "1", "GOW3_ACB_DISPATCH_PIPE" to "1", "GOW3_READBACK_SIDEQ" to "1",
        ), "Playable (Galaxy S24 Ultra)"),
        Game("uc3", "Uncharted 3: Drake's Deception", "big3-ps4_Shipping.elf", COMMON + mapOf(
            "GOW3_COMPUTE_PIPE" to "1", "GOW3_SYNC_SMALL_TARGETS" to "1", "GOW3_MIP_FIX" to "1",
            "GOW3_SCOPE_SPLIT_SCALAR" to "1", "GOW3_DEPTH_KEEP_CLEAR" to "1",
        ), "Playable (Galaxy S24 Ultra)"),
    )

    /** The collection: its folder holds the Uncharted 2 and 3 executables next to eboot.bin. */
    @JvmStatic
    fun isCollection(game: File?): Boolean =
        game != null && File(game, "big3-ps4_Shipping.elf").isFile && File(game, "big2-ps4_Shipping.elf").isFile

    /** The game chosen in gow3.ini (uc_game), or null when the folder is not the collection. */
    @JvmStatic
    fun selected(game: File?, ini: Map<String, String>): Game? {
        if (!isCollection(game)) return null
        val key = ini["uc_game"]?.trim()?.lowercase() ?: "uc3"
        return GAMES.firstOrNull { it.key == key } ?: GAMES.last()
    }
}
