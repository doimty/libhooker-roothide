//
//  CoreServices.h
//  libhooker configurator
//
//  Created by CoolStar on 1/25/21.
//  Copyright © 2021 coolstar. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>

#ifndef CoreServices_h
#define CoreServices_h

// RootHide resolves its randomized bootstrap through libroothide, never a path scan.
FOUNDATION_EXPORT NSString * _Nonnull LHRootPath(NSString * _Nonnull path);
FOUNDATION_EXPORT NSString * _Nonnull LHRootFSPath(NSString * _Nonnull path);

@interface LSApplicationProxy : NSObject
- (nullable NSURL *)bundleURL;
- (nullable NSString *)localizedName;
- (nullable NSString *)applicationIdentifier;
- (nullable NSString *)_boundApplicationIdentifier;
@end

@interface LSApplicationProxy (libhooker)
- (nullable NSString *)LHIdentifier;
@end

@interface LSApplicationWorkspace : NSObject
+ (nonnull instancetype)defaultWorkspace;
- (nonnull NSArray<LSApplicationProxy *> *)allInstalledApplications;
@end

#endif /* CoreServices_h */
