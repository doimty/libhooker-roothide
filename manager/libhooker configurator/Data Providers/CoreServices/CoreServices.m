//
//  CoreServices.m
//  libhooker configurator
//
//  Created by CoolStar on 1/25/21.
//  Copyright © 2021 coolstar. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "CoreServices.h"
#if LHC_ROOTHIDE
#import <roothide/roothide.h>
#endif

NSString *LHRootPath(NSString *path) {
#if LHC_ROOTHIDE
    return jbroot(path);
#else
    return path;
#endif
}

NSString *LHRootFSPath(NSString *path) {
#if LHC_ROOTHIDE
    return rootfs(path);
#else
    return path;
#endif
}

@implementation LSApplicationProxy (libhooker)
- (NSString *)LHIdentifier {
    if ([self respondsToSelector:@selector(_boundApplicationIdentifier)]) {
        NSString *identifier = [self _boundApplicationIdentifier];
        if (identifier.length) return identifier;
    }
    return [self applicationIdentifier];
}
@end
