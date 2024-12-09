//
//  DeviceUtils.m
//  AneAwesomeUtils-IOS
//
//  Created by João Vitor Borges on 03/11/24.
//

#import <Foundation/Foundation.h>
#include "DeviceUtils.h"
#include <TargetConditionals.h> // Ensure TARGET_OS_IOS is defined
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#endif

char* getDeviceUniqueId(void) {
#if TARGET_OS_IOS
    NSString *uuidString = [[UIDevice currentDevice] identifierForVendor].UUIDString;
    return strdup([uuidString UTF8String]); // Copy string to ensure stability
#else
    return NULL;
#endif
}
