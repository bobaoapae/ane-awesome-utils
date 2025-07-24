//
// Created by reveny on 29/08/2023.
//
#include "Headers/EmulatorDetection.hpp"

extern "C" {
    JNIEXPORT jboolean JNICALL
    Java_br_com_redesurftank_aneawesomeutils_EmulatorDetection_isDetected(JNIEnv *env, jobject clazz) {
        (void)env;
        (void)clazz;

        return EmulatorDetection::isDetected();
    }

    JNIEXPORT jstring JNICALL
    Java_br_com_redesurftank_aneawesomeutils_EmulatorDetection_getResult(JNIEnv *env, jobject clazz) {
        (void)clazz;

        return env->NewStringUTF(EmulatorDetection::getResult().c_str());
    }
}