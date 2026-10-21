package com.maya.assistant

/** Receives events from the native engine. Called on native worker threads: do not touch views here. */
interface MayaListener {
    fun onEvent(type: Int, payload: ByteArray?)
}

/**
 * JNI surface of libmaya.so (see app/src/main/cpp/src/maya_jni.cpp).
 * The library registers these methods itself in JNI_OnLoad, so loading it must happen from this class.
 */
object NativeBridge {
    const val EVT_STATE = 1
    const val EVT_WAKE = 2
    const val EVT_TRANSCRIPT = 3
    const val EVT_REPLY_DELTA = 4
    const val EVT_REPLY_DONE = 5
    const val EVT_ERROR = 6
    const val EVT_INFO = 7
    const val EVT_METRIC = 8

    const val STATE_STOPPED = 0
    const val STATE_PASSIVE = 1
    const val STATE_LISTENING = 2
    const val STATE_TRANSCRIBING = 3
    const val STATE_THINKING = 4
    const val STATE_SPEAKING = 5
    const val STATE_ERROR = 6

    init {
        System.loadLibrary("maya")
    }

    /** Loads every model (slow, call off the main thread). Returns a handle, or 0 on failure (see EVT_ERROR). */
    external fun nativeCreate(config: String, listener: MayaListener): Long
    external fun nativeStart(handle: Long): Boolean
    external fun nativeStop(handle: Long)
    external fun nativeInterrupt(handle: Long)
    external fun nativeDestroy(handle: Long)

    /** Wake-word calibration: "OK\t<heard text>\t<tokens>" or "ERR\t<message>". */
    external fun nativeCalibrateDecode(config: String, pcm: ShortArray): String
}
