#include <android/log.h>
#include <android/native_activity.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <expected>
#include <jni.h>
#include <pthread.h>
#include <SLES/OpenSLES.h>
#include <stdexcept>
#include <time.h>

static_assert(__cplusplus >= 202302L);
namespace {
int constructed{}, loaded{};
thread_local int tls_value = 7;
pthread_key_t thread_key;
int wide_format(wchar_t *output, size_t capacity, const wchar_t *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    auto result = std::vswprintf(output, capacity, format, arguments);
    va_end(arguments);
    return result;
}
int formatting_contract() {
    static_assert(sizeof(wchar_t) == 4);
    wchar_t output[64]{};
    if (wide_format(output, 64, L"%ls:%lld:%lc:%.2f", L"seed", 4294967298ll, 0x1f331u, 1.25) != 22 ||
        std::wcscmp(output, L"seed:4294967298:\U0001f331:1.25") != 0)
        return 60;
    if (std::swprintf(output, 3, L"%d", 1234) >= 0 || errno != EOVERFLOW || output[2] != 0)
        return 61;
    if (std::swprintf(output, 0, L"x") >= 0 || errno != EINVAL)
        return 62;
    return 0;
}
void object_realized(SLObjectItf, const void *context, SLuint32 event, SLresult result, SLuint32 state,
                     void *interface) {
    if (event == SL_OBJECT_EVENT_ASYNC_TERMINATION && result == SL_RESULT_SUCCESS &&
        state == SL_OBJECT_STATE_REALIZED && !interface)
        ++*static_cast<int *>(const_cast<void *>(context));
}
int audio_contract() {
    SLObjectItf engine_object{}, mix{}, player{};
    int realized{};
    if (slCreateEngine(&engine_object, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
        (*engine_object)->RegisterCallback(engine_object, object_realized, &realized) !=
            SL_RESULT_SUCCESS ||
        (*engine_object)->Realize(engine_object, SL_BOOLEAN_TRUE) != SL_RESULT_SUCCESS || realized != 1)
        return 50;
    SLEngineItf engine{};
    if ((*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine) != SL_RESULT_SUCCESS ||
        (*engine)->CreateOutputMix(engine, &mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
        (*mix)->Realize(mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
        return 51;
    SLDataLocator_BufferQueue locator{SL_DATALOCATOR_BUFFERQUEUE, 2};
    SLDataFormat_PCM format{SL_DATAFORMAT_PCM,           1,
                            SL_SAMPLINGRATE_44_1,        SL_PCMSAMPLEFORMAT_FIXED_16,
                            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER,
                            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source{&locator, &format};
    SLDataLocator_OutputMix output{SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink{&output, nullptr};
    SLInterfaceID ids[]{SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
    SLboolean required[]{SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    if ((*engine)->CreateAudioPlayer(engine, &player, &source, &sink, 2, ids, required) !=
            SL_RESULT_SUCCESS ||
        (*player)->Realize(player, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
        return 52;
    SLBufferQueueItf queue{};
    SLVolumeItf volume{};
    SLPlayItf play{};
    if ((*player)->GetInterface(player, SL_IID_BUFFERQUEUE, &queue) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_VOLUME, &volume) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_PLAY, &play) != SL_RESULT_SUCCESS)
        return 53;
    SLmillibel level{};
    if ((*volume)->SetVolumeLevel(volume, -1000) != SL_RESULT_SUCCESS ||
        (*volume)->GetVolumeLevel(volume, &level) != SL_RESULT_SUCCESS || level != -1000)
        return 54;
    const short samples[16]{};
    if ((*queue)->Enqueue(queue, samples, sizeof(samples)) != SL_RESULT_SUCCESS)
        return 55;
    SLBufferQueueState state{};
    if ((*queue)->GetState(queue, &state) != SL_RESULT_SUCCESS || state.count != 1 ||
        state.playIndex != 0 || (*queue)->Clear(queue) != SL_RESULT_SUCCESS ||
        (*queue)->GetState(queue, &state) != SL_RESULT_SUCCESS || state.count != 0)
        return 56;
    if ((*play)->SetPlayState(play, SL_PLAYSTATE_PAUSED) != SL_RESULT_SUCCESS ||
        (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED) != SL_RESULT_SUCCESS)
        return 57;
    (*player)->Destroy(player);
    (*mix)->Destroy(mix);
    (*engine_object)->Destroy(engine_object);
    return 0;
}

__attribute__((constructor)) void constructor() {
    ++constructed;
}

std::expected<int, int> positive(int value) {
    if (value < 0)
        return std::unexpected(17);
    return value;
}
int compare(const void *left, const void *right) {
    const auto a = *static_cast<const int *>(left), b = *static_cast<const int *>(right);
    return (a > b) - (a < b);
}
struct Work {
    JavaVM *vm{};
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    bool done{};
    int result{};
};
void *worker(void *argument) {
    auto &work = *static_cast<Work *>(argument);
    JNIEnv *env{};
    int result = 42;
    if (tls_value != 7 || pthread_getspecific(thread_key))
        result = 31;
    tls_value = 99;
    pthread_setspecific(thread_key, reinterpret_cast<void *>(42));
    if (pthread_getspecific(thread_key) != reinterpret_cast<void *>(42))
        result = 32;
    if (work.vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_EDETACHED)
        result = 33;
    if (work.vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)
        result = 34;
    else {
        if (env->ExceptionCheck())
            result = 37;
        auto text = env->NewStringUTF("worker");
        if (env->GetStringLength(text) != 6)
            result = 35;
        env->DeleteLocalRef(text);
        auto exception_type = env->FindClass("java/lang/RuntimeException");
        env->ThrowNew(exception_type, "worker exception");
        if (!env->ExceptionCheck())
            result = 38;
        env->ExceptionClear();
        env->DeleteLocalRef(exception_type);
        if (work.vm->DetachCurrentThread() != JNI_OK)
            result = 36;
    }
    pthread_mutex_lock(&work.mutex);
    work.result = result;
    work.done = true;
    pthread_cond_signal(&work.condition);
    pthread_mutex_unlock(&work.mutex);
    return reinterpret_cast<void *>(static_cast<long>(result));
}
} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env{};
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return JNI_ERR;
    ++loaded;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT int acp_native_test(JNIEnv *env, const char *save_path) {
    if (auto formatting = formatting_contract())
        return formatting;
    if (auto audio = audio_contract())
        return audio;
    if (constructed != 1 || (loaded != 0 && loaded != 1))
        return 1;
    if (positive(42).value_or(0) != 42 || positive(-1).error() != 17)
        return 2;
    bool caught = false;
    try {
        throw std::runtime_error("ARM64 C++ exception");
    } catch (const std::exception &error) {
        caught = std::strcmp(error.what(), "ARM64 C++ exception") == 0;
    }
    if (!caught)
        return 3;
    const jchar utf16[] = {'A', 0, 0xd83c, 0xdf31};
    auto text = env->NewString(utf16, 4);
    if (env->GetStringLength(text) != 4 || env->GetStringUTFLength(text) != 9)
        return 4;
    auto type = env->GetObjectClass(text);
    auto method = env->GetMethodID(type, "length", "()I");
    if (env->CallIntMethod(text, method) != 4)
        return 5;
    auto global = env->NewGlobalRef(text);
    auto alias = env->NewLocalRef(global);
    if (!env->IsSameObject(text, alias))
        return 6;
    env->DeleteGlobalRef(global);
    env->DeleteLocalRef(text);
    if (env->GetStringLength(static_cast<jstring>(alias)) != 4)
        return 7;
    env->DeleteLocalRef(alias);
    env->DeleteLocalRef(type);
    auto array = env->NewIntArray(3);
    const jint values[] = {19, 23, 42};
    env->SetIntArrayRegion(array, 0, 3, values);
    auto elements = env->GetIntArrayElements(array, nullptr);
    elements[2] = elements[0] + elements[1];
    env->ReleaseIntArrayElements(array, elements, 0);
    jint answer{};
    env->GetIntArrayRegion(array, 2, 1, &answer);
    env->DeleteLocalRef(array);
    if (answer != 42)
        return 8;
    if (env->PushLocalFrame(4) != JNI_OK)
        return 9;
    auto framed = env->NewStringUTF("retained");
    auto retained = static_cast<jstring>(env->PopLocalFrame(framed));
    if (env->GetStringLength(retained) != 8)
        return 10;
    env->DeleteLocalRef(retained);
    auto exception_type = env->FindClass("java/lang/RuntimeException");
    env->ThrowNew(exception_type, "expected fixture exception");
    if (!env->ExceptionCheck())
        return 11;
    auto exception = env->ExceptionOccurred();
    env->ExceptionClear();
    env->DeleteLocalRef(exception);
    env->DeleteLocalRef(exception_type);
    int unsorted[] = {42, -7, 8, 0};
    std::qsort(unsorted, 4, sizeof(int), compare);
    if (unsorted[0] != -7 || unsorted[3] != 42)
        return 12;
    tls_value = 8;
    Work work;
    if (env->GetJavaVM(&work.vm) != JNI_OK || pthread_key_create(&thread_key, nullptr))
        return 13;
    auto pending_type = env->FindClass("java/lang/RuntimeException");
    env->ThrowNew(pending_type, "main thread exception");
    pthread_mutex_lock(&work.mutex);
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, worker, &work))
        return 14;
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    while (!work.done) {
        if (pthread_cond_timedwait(&work.condition, &work.mutex, &deadline)) {
            pthread_mutex_unlock(&work.mutex);
            pthread_join(thread, nullptr);
            return 15;
        }
    }
    pthread_mutex_unlock(&work.mutex);
    void *result{};
    if (pthread_join(thread, &result) || result != reinterpret_cast<void *>(42) || work.result != 42)
        return work.result ? work.result : 16;
    if (!env->ExceptionCheck())
        return 39;
    env->ExceptionClear();
    env->DeleteLocalRef(pending_type);
    if (tls_value != 8 || pthread_getspecific(thread_key))
        return 18;
    pthread_key_delete(thread_key);
    pthread_mutex_destroy(&work.mutex);
    pthread_cond_destroy(&work.condition);
    constexpr char marker[] = "C++23 ARM64 JNI callbacks TLS exceptions saves: 42\n";
    if (auto previous = std::fopen(save_path, "rb")) {
        char bytes[sizeof(marker)]{};
        const auto count = std::fread(bytes, 1, sizeof(marker) - 1, previous);
        std::fclose(previous);
        if (count != sizeof(marker) - 1 || std::memcmp(bytes, marker, count))
            return 19;
    }
    auto file = std::fopen(save_path, "wb");
    if (!file)
        return 20;
    const auto count = std::fwrite(marker, 1, sizeof(marker) - 1, file);
    const auto close = std::fclose(file);
    return count == sizeof(marker) - 1 && close == 0 ? 0 : 21;
}

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *activity, void *, size_t) {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s/native-test-save.txt", activity->internalDataPath);
    const auto result = acp_native_test(activity->env, path);
    __android_log_print(result ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "ACP.NativeTest", "result=%d",
                        result);
}
