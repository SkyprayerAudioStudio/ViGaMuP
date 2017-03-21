#include <jni.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
#include <android/log.h>

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "libkss/src/kssplay.h"

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;


// aux effect on the output mix, used by the buffer queue player
static const SLEnvironmentalReverbSettings reverbSettings =
        SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;

/*// URI player interfaces
static SLObjectItf uriPlayerObject = NULL;
static SLPlayItf uriPlayerPlay;
static SLSeekItf uriPlayerSeek;
static SLMuteSoloItf uriPlayerMuteSolo;
static SLVolumeItf uriPlayerVolume;

// file descriptor player interfaces
static SLObjectItf fdPlayerObject = NULL;
static SLPlayItf fdPlayerPlay;
static SLSeekItf fdPlayerSeek;
static SLMuteSoloItf fdPlayerMuteSolo;
static SLVolumeItf fdPlayerVolume;
*/
// recorder interfaces
/*static SLObjectItf recorderObject = NULL;
static SLRecordItf recorderRecord;
static SLAndroidSimpleBufferQueueItf recorderBufferQueue;*/

// pointer and size of the next player buffer to enqueue, and number of remaining buffers
static short *nextBuffer;
static unsigned nextSize;
static int nextCount;

KSSPLAY *kssplay;
KSS *kss;
FILE *fp;

int secondsToGenerate = 0;
int trackLength = 0;
int initialDataCopied = 0;
int generatingAllowed = 0;
char *currentFile = "";
int queueSecond = 0;
int secondsPlayed = 0;
int isPlaying = 0;
int isPaused = 0;
int isBuffering = 0;
int16_t *fullTrackWavebuf;
int16_t *wavebuf;
int16_t *wavebuf2;
int nextMessageSend = 0;

pthread_mutex_t lock;

int16_t *queueBuffer1, *queueBuffer2, *queueBuffer3;
int queueBufferToUse = 1;

// processing callback to handler class
typedef struct tick_context {
    JavaVM  *javaVM;
    jclass   PlayerServiceClz;
    jobject  PlayerServiceObj;
    pthread_mutex_t  lock ;
} TickContext;
TickContext g_ctx;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    __android_log_print(ANDROID_LOG_INFO, "KSS", "JNI onload!");

    JNIEnv* env;
    memset(&g_ctx, 0, sizeof(g_ctx));

    g_ctx.javaVM = vm;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR; // JNI version not supported.
    }

    g_ctx.PlayerServiceObj = NULL;
    return  JNI_VERSION_1_6;
}

// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    queueSecond = 1;
    generatingAllowed = 1;
}

int queueSecondIfRequired(){
    if (queueSecond && secondsPlayed<trackLength) {
        if (queueBufferToUse == 1) {
            //__android_log_print(ANDROID_LOG_INFO, "KSS", "queuing a second! %d", secondsPlayed);
            memcpy(queueBuffer1, (int8_t *) &fullTrackWavebuf[secondsPlayed * 48000], 96000);
            (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, queueBuffer1, 96000);
            queueSecond = 0;
            queueBufferToUse++;
            secondsPlayed++;
        }
        if (queueBufferToUse == 2 && queueSecond != 0) {
            //__android_log_print(ANDROID_LOG_INFO, "KSS", "queuing a second! %d", secondsPlayed);
            memcpy(queueBuffer2, (int8_t *) &fullTrackWavebuf[secondsPlayed * 48000], 96000);
            (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, queueBuffer2, 96000);
            queueSecond = 0;
            queueBufferToUse++;
            secondsPlayed++;
        }
        if (queueBufferToUse == 3 && queueSecond != 0) {
            //__android_log_print(ANDROID_LOG_INFO, "KSS", "queuing a second! %d", secondsPlayed);
            memcpy(queueBuffer3, (int8_t *) &fullTrackWavebuf[secondsPlayed * 48000], 96000);
            (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, queueBuffer3, 96000);
            queueSecond = 0;
            queueBufferToUse = 1;
            secondsPlayed++;
        }
        return 1;
    }
    return 0;
}

void* generateAudioThread(void* context){
    TickContext *pctx = (TickContext*) context;
    JavaVM *javaVM = pctx->javaVM;
    JNIEnv *env;
    jint res = (*javaVM)->GetEnv(javaVM, (void**)&env, JNI_VERSION_1_6);
    if (res != JNI_OK) {
        res = (*javaVM)->AttachCurrentThread(javaVM, &env, NULL);
        __android_log_print(ANDROID_LOG_INFO, "KSS", "Attaching!!");
        if (JNI_OK != res) {
            __android_log_print(ANDROID_LOG_INFO, "KSS", "Failed to AttachCurrentThread, ErrorCode = %d", res);
        }
    }
    jmethodID nextTrackId = (*env)->GetMethodID(env, pctx->PlayerServiceClz, "nextTrack", "()V");
    jmethodID setBufferBarProgressId = (*env)->GetMethodID(env, pctx->PlayerServiceClz, "setBufferBarProgress", "()V");
    jmethodID setSeekBarThumbProgressId = (*env)->GetMethodID(env, pctx->PlayerServiceClz, "setSeekBarThumbProgress", "()V");
    while(1)
    {
        isBuffering = 1;
        while (secondsToGenerate > 0 && generatingAllowed == 1 && isPlaying){
            if (!initialDataCopied){
                //__android_log_print(ANDROID_LOG_INFO, "KSS", "copy initial data to array!");
                initialDataCopied=1;
                memcpy(fullTrackWavebuf,wavebuf,96000);
                memcpy((int8_t *)&fullTrackWavebuf[48000],wavebuf2,96000);
                secondsPlayed = 2;
                secondsToGenerate = secondsToGenerate - 2;
            }
            KSSPLAY_calc(kssplay, (int8_t *)&fullTrackWavebuf[(trackLength-secondsToGenerate)*48000], 48000);
            (*env)->CallVoidMethod(env, pctx->PlayerServiceObj, setBufferBarProgressId);
            secondsToGenerate--;
            if (secondsToGenerate==1) KSSPLAY_fade_start(kssplay, 1000); //todo: only fade when song loops...
            if (queueSecondIfRequired()==1) (*env)->CallVoidMethod(env, pctx->PlayerServiceObj, setSeekBarThumbProgressId);
            nextMessageSend = 0;
        }
        isBuffering=0;
        if (queueSecondIfRequired()==1) (*env)->CallVoidMethod(env, pctx->PlayerServiceObj, setSeekBarThumbProgressId);
        if (isPlaying && !nextMessageSend && secondsPlayed==trackLength && trackLength!=0) {
            __android_log_print(ANDROID_LOG_INFO, "KSS", "is playing! %d %d %d %d", isPlaying, nextMessageSend, secondsPlayed, trackLength);
            sleep (2);

            (*env)->CallVoidMethod(env, pctx->PlayerServiceObj, nextTrackId);
            //(*javaVM)->DetachCurrentThread(javaVM);

            nextMessageSend = 1;
        }
        usleep (500);
    }
    // auto next track when ended
    // report generation to android app
}

void Java_nl_vlessert_vigamup_PlayerService_setKssProgress(JNIEnv* env, jclass clazz, int progress){
    pthread_mutex_lock(&lock);
    //(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
    //free(fullTrackWavebuf);
    //SLresult result = (*bqPlayerBufferQueue)->Clear(bqPlayerBufferQueue);
    //if (SL_RESULT_SUCCESS != result) {
       // __android_log_print(ANDROID_LOG_INFO, "KSS", "Clear failed at begin of OpenSL_startRecording()() result=%d\n", result);
        //return;
    //}
    secondsPlayed=progress;
    //(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    pthread_mutex_unlock(&lock);
}

void Java_nl_vlessert_vigamup_PlayerService_setKss(JNIEnv* env, jclass clazz, char *file){
    const char *utf8File = (*env)->GetStringUTFChars(env, file, NULL);
    // fix would be nice... if changing game don't reset everything...
    if (isPlaying) {
        isPlaying=0;
        while (isBuffering){
            usleep(100);
        }
        pthread_mutex_lock(&lock);
        //(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
        //free(fullTrackWavebuf);
        SLresult result = (*bqPlayerBufferQueue)->Clear(bqPlayerBufferQueue);
        if (SL_RESULT_SUCCESS != result) {
            __android_log_print(ANDROID_LOG_INFO, "KSS", "Clear failed at begin of OpenSL_startRecording()() result=%d\n", result);
            return;
        }
        //(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
        pthread_mutex_unlock(&lock);
    }

    if (strcmp(utf8File,currentFile)!=0) {
        if (strcmp("", currentFile)!=0) {
            KSSPLAY_delete(kssplay);
            KSS_delete(kss);
        }
        __android_log_print(ANDROID_LOG_INFO, "KSS", "Loading KSS... %s",utf8File);
        //char *file = "/sdcard/Download/ViGaMuP/KSS/quarth.kss";

        if ((kss = KSS_load_file(utf8File)) == NULL) {
            __android_log_print(ANDROID_LOG_INFO, "KSS", "Error loading... KSS...");
        }
        kssplay = KSSPLAY_new(48000, 1, 16); // so frequency (48k) * channels (1) * bitrate (16) = 96000 bytes per second
        KSSPLAY_set_data(kssplay, kss);
        KSSPLAY_set_master_volume(kssplay,100);
        currentFile = file;

        __android_log_print(ANDROID_LOG_INFO, "KSS", "FMPAC enabled: %d", kss->fmpac);

        queueBuffer1 = malloc(96000);
        queueBuffer2 = malloc(96000);
        queueBuffer3 = malloc(96000);
    }

}

void Java_nl_vlessert_vigamup_PlayerService_togglePlayback(JNIEnv* env, jclass clazz) {
    if (isPaused) {
        (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
        isPaused = 0;
    } else {
        (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PAUSED);
        isPaused = 1;
    }
}

void Java_nl_vlessert_vigamup_PlayerService_setKssTrack(JNIEnv* env, jclass clazz, int trackNr, int secondsToPlay){
    secondsToGenerate = 0;
    trackLength = 0;
    initialDataCopied = 0;
    generatingAllowed = 0;
    queueSecond = 0;
    secondsPlayed = 0;

    if (isPlaying) {
        isPlaying=0;
        while (isBuffering){
            usleep(100);
        }
        pthread_mutex_lock(&lock);
        //(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
        //free(fullTrackWavebuf);
        SLresult result = (*bqPlayerBufferQueue)->Clear(bqPlayerBufferQueue);
        if (SL_RESULT_SUCCESS != result) {
            __android_log_print(ANDROID_LOG_INFO, "KSS", "Clear failed at begin of OpenSL_startRecording()() result=%d\n", result);
            return;
        }
        //(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
        pthread_mutex_unlock(&lock);
    }

    KSSPLAY_reset(kssplay, trackNr, 0);
    secondsToGenerate = secondsToPlay;
    if (secondsToPlay==1) secondsToPlay=3;
    trackLength = secondsToPlay;
    fullTrackWavebuf = malloc(96000 * secondsToPlay);
    wavebuf = malloc(48000 * sizeof(int16_t));
    wavebuf2 = malloc(96000);
    KSSPLAY_calc(kssplay, wavebuf, 48000);
    KSSPLAY_calc(kssplay, wavebuf2, 48000);
    (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, wavebuf, 96000);
    (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, wavebuf2, 96000);
    isPlaying = 1;
}

// create the engine and output mix objects
void Java_nl_vlessert_vigamup_PlayerService_createEngine(JNIEnv* env, jobject instance)
{
    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // create output mix, with environmental reverb specified as a non-required interface
    //const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    //const SLInterfaceID ids[1] = {SL_IID_VOLUME};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, NULL, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the environmental reverb interface
    // this could fail if the environmental reverb effect is not available,
    // either because the feature is not present, excessive CPU load, or
    // the required MODIFY_AUDIO_SETTINGS permission was not requested and granted
    /*result = (*outputMixObject)->GetInterface(outputMixObject, SL_IID_ENVIRONMENTALREVERB,
                                              &outputMixEnvironmentalReverb);
    if (SL_RESULT_SUCCESS == result) {
        result = (*outputMixEnvironmentalReverb)->SetEnvironmentalReverbProperties(
                outputMixEnvironmentalReverb, &reverbSettings);
        (void)result;
    }*/
    // ignore unsuccessful result codes for environmental reverb, as it is optional for this example

    pthread_attr_t  threadAttr_;

    pthread_attr_init(&threadAttr_);
    pthread_attr_setdetachstate(&threadAttr_, PTHREAD_CREATE_DETACHED);

    pthread_mutex_init(&g_ctx.lock, NULL);

    jclass clz = (*env)->GetObjectClass(env, instance);
    g_ctx.PlayerServiceClz = (*env)->NewGlobalRef(env, clz);
    g_ctx.PlayerServiceObj = (*env)->NewGlobalRef(env, instance);

    pthread_t t1;
    pthread_create(&t1,&threadAttr_,generateAudioThread,&g_ctx);

    /*jclass cls = (*env)->GetObjectClass(env, instance);

    jmethodID callbackMethod = (*env)->GetMethodID(env, cls, "nextTrack", "()V");

    (*env)->CallVoidMethod(env, instance, callbackMethod);*/

    //pthread_join(t1,NULL);
}

// create buffer queue audio player
void Java_nl_vlessert_vigamup_PlayerService_createBufferQueueAudioPlayer(JNIEnv* env,
                                                                           jclass clazz, jint sampleRate, jint bufSize) {
    SLresult result;
    if (sampleRate >= 0 && bufSize >= 0) {
        bqPlayerSampleRate = sampleRate * 1000;
        /*
         * device native buffer size is another factor to minimize audio latency, not used in this
         * sample: we only play one giant buffer here
         */
        //bqPlayerBufSize = bufSize;
    }

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_8,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the native, fast audio path
     * will be triggered
     */
    if (bqPlayerSampleRate) {
        format_pcm.samplesPerSec = bqPlayerSampleRate;       //sample rate in mili second
    }
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    /*
     * create audio player:
     *     fast audio does not support when SL_IID_EFFECTSEND is required, skip it
     *     for fast audio case
     */
    const SLInterfaceID ids[3] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME, SL_IID_EFFECTSEND,
            /*SL_IID_MUTESOLO,*/};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/ };

    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk,
                                                bqPlayerSampleRate ? 2 : 3, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the play interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the buffer queue interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
                                             &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the effect send interface
    bqPlayerEffectSend = NULL;
    if (0 == bqPlayerSampleRate) {
        result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
                                                 &bqPlayerEffectSend);
        assert(SL_RESULT_SUCCESS == result);
        (void) result;
    }

#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

    // get the volume interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
}

// shut down the native audio system
void Java_nl_vlessert_vigamup_PlayerService_shutdown(JNIEnv* env, jclass clazz)
{

    // destroy buffer queue audio player object, and invalidate all associated interfaces
    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerEffectSend = NULL;
        bqPlayerMuteSolo = NULL;
        bqPlayerVolume = NULL;
    }

    // destroy file descriptor audio player object, and invalidate all associated interfaces
    /*if (fdPlayerObject != NULL) {
        (*fdPlayerObject)->Destroy(fdPlayerObject);
        fdPlayerObject = NULL;
        fdPlayerPlay = NULL;
        fdPlayerSeek = NULL;
        fdPlayerMuteSolo = NULL;
        fdPlayerVolume = NULL;
    }*/

    // destroy URI audio player object, and invalidate all associated interfaces
    /*if (uriPlayerObject != NULL) {
        (*uriPlayerObject)->Destroy(uriPlayerObject);
        uriPlayerObject = NULL;
        uriPlayerPlay = NULL;
        uriPlayerSeek = NULL;
        uriPlayerMuteSolo = NULL;
        uriPlayerVolume = NULL;
    }*/

    // destroy audio recorder object, and invalidate all associated interfaces
    /*if (recorderObject != NULL) {
        (*recorderObject)->Destroy(recorderObject);
        recorderObject = NULL;
        recorderRecord = NULL;
        recorderBufferQueue = NULL;
    }*/

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
        //outputMixEnvironmentalReverb = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }
}

void investigateTrackFurther(int tracknr){
    int16_t *generateWavebuf  = malloc (96000*600); //10 minutes seems enough to detect loops and for every other song to end....
    int16_t *tempWavebuf = malloc(96000);
    int secondsCalculated = 0;
    char fn[101];
    FILE *f;

    KSSPLAY_reset(kssplay, tracknr, 0);
    // loop detection: if second >1 compare with first second, if same loop detected
    // non loop song: detect silence
    while (1){
        KSSPLAY_calc(kssplay, tempWavebuf, 48000);
        //__android_log_print(ANDROID_LOG_INFO, "KSS", "Calculated %d...\n", secondsCalculated);
        if (secondsCalculated>0){
            if (memcmp(generateWavebuf, tempWavebuf, 44100)==0) {
                __android_log_print(ANDROID_LOG_INFO, "KSS", "Loop detected in %d on second %d!!\n", tracknr, secondsCalculated);
                break;
            }
        }
        memcpy((int8_t *) &generateWavebuf[secondsCalculated * 48000], tempWavebuf, 96000);
        snprintf(fn, 100, "/sdcard/Download/%d.txt", secondsCalculated);
        f =  fopen(fn, "wb");
        fwrite (tempWavebuf, 1, 96000, f);
        fclose(f);
        __android_log_print(ANDROID_LOG_INFO, "KSS", "Calculated %d...\n", secondsCalculated);
        secondsCalculated++;
        if (secondsCalculated==50) break;
    }
    f = fopen("/sdcard/Download/1.wav", "wb");
    fwrite (generateWavebuf, 1, 50*96000, f);
    fclose (f);
}

void Java_nl_vlessert_vigamup_PlayerService_generateTrackInformation(JNIEnv* env, jclass clazz){
    __android_log_print(ANDROID_LOG_INFO, "KSS", "Generating trackinformation... \n");
    int16_t *tempWavebuf;
    int16_t *tempWavebuf2;
    int16_t *tempWavebuf3;
    int printedBefore = 0;
    //for (int i = 0; i<=255; i++){
        tempWavebuf = malloc (96000); // 1 second
        tempWavebuf2 = malloc (96000); // 1 second
        tempWavebuf3 = malloc (96000); // 1 second
        int i = 72;
        KSSPLAY_reset(kssplay, i, 0);
        //__android_log_print(ANDROID_LOG_INFO, "KSS", "Checking track %d\n", i);
        KSSPLAY_calc(kssplay, tempWavebuf, 48000);
        int sum = 0, j = 0;
        for (j = 0; j < 44100; ++j) {
            sum += tempWavebuf[j];
        }
        if (sum != 0) {
            //__android_log_print(ANDROID_LOG_INFO, "KSS", "Sound detected in track %d (%d)\n", i, sum);
            KSSPLAY_calc(kssplay, tempWavebuf2, 48000);
            if (memcmp(tempWavebuf, tempWavebuf2, 96000)!=0){
                //__android_log_print(ANDROID_LOG_INFO, "KSS", "Sound found in track %d; second 1 == second 2, probably sfx...\n", i);
            //} else {
                KSSPLAY_calc(kssplay, tempWavebuf3, 48000);
                KSSPLAY_calc(kssplay, tempWavebuf3, 48000);
                KSSPLAY_calc(kssplay, tempWavebuf3, 48000);
                //FILE *f =  fopen("/sdcard/Download/text.txt", "wb");
                //fwrite (tempWavebuf3, 1, 96000, f);
                //fclose(f);
                sum = 0;
                for (j = 0; j < 44100; ++j) { //understand it's 16 bit int!!
                    sum += tempWavebuf3[j];
                    /*if (j == 1000 || j == 44099){
                        //if (!printedBefore) {
                            __android_log_print(ANDROID_LOG_INFO, "KSS", "add, waarde %d positie %d\n", sum, j);
                            //printedBefore=1;
                        //}
                    }*/
                }
                if (sum != 0) {
                    __android_log_print(ANDROID_LOG_INFO, "KSS", "Sound found in track %d, sum of generated 5th second = %d; should be music!!...\n", i, sum);
                    investigateTrackFurther(i);
                }
            }
        //} else {
            //__android_log_print(ANDROID_LOG_INFO, "KSS", "Empty track... skipping %d\n",i);
        }
        free (tempWavebuf);
        free (tempWavebuf2);
        free (tempWavebuf3);
    //}
}

